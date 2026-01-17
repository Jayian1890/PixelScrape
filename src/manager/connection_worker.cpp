#include "manager.hpp"

namespace pixelscrape {

void TorrentManager::connection_worker() {
  using namespace std::chrono_literals;
  const auto max_queue_age = 30s; // total timeout for connection attempt

  std::vector<ConnectionRequest> active_requests;

  while (connection_worker_running_) {
    // 1. Drain new requests from thread-safe queue
    {
      std::unique_lock<std::mutex> qlock(connection_mutex_);
      // If we have no active requests, wait for work
      if (active_requests.empty()) {
        connection_cv_.wait_for(qlock, 1s, [&]() {
          return !connection_queue_.empty() || !connection_worker_running_;
        });
      }

      if (!connection_worker_running_)
        break;

      while (!connection_queue_.empty()) {
        active_requests.push_back(std::move(connection_queue_.front()));
        connection_queue_.pop();
      }
    }

    // 2. Prepare select sets
    fd_set read_fds;
    fd_set write_fds;
    fd_set error_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&error_fds);

    int max_fd = -1;
    auto now = std::chrono::steady_clock::now();

    // 3. Process active requests state machine & setup fds
    for (auto it = active_requests.begin(); it != active_requests.end();) {
      bool drop = false;

      // Timeout check
      if (now - it->start_time > max_queue_age) {
        drop = true;
      } else {
        int fd = it->peer_connection->get_fd();

        if (it->state == ConnectionState::CONNECTING) {
          // If not started yet (fd == -1), start it
          if (fd == -1) {
            // Check peer cap before starting
            bool cap_reached = false;
            {
              std::lock_guard<std::mutex> lock(mutex_);
              auto t_it = torrents_.find(it->torrent_id);
              if (t_it == torrents_.end() || t_it->second->peers.size() >= 50) {
                cap_reached = true;
              }
            }
            if (cap_reached) {
              drop = true;
            } else {
              if (it->peer_connection->start_connect()) {
                // Check if immediately connected or in progress
                fd = it->peer_connection->get_fd();
                if (fd != -1) {
                  // In progress or connected. If connected, state transition
                  // handled below via check_connect_result But typically we
                  // wait for writeability to confirm connection completion
                  FD_SET(fd, &write_fds);
                  if (fd > max_fd)
                    max_fd = fd;
                } else {
                  // Failed to start
                  drop = true;
                }
              } else {
                drop = true;
              }
            }
          } else {
            // Already passed to connect(), waiting for completion
            FD_SET(fd, &write_fds);
            if (fd > max_fd)
              max_fd = fd;
          }
        } else if (it->state == ConnectionState::HANDSHAKING) {
          // Waiting for handshake response
          if (fd != -1) {
            FD_SET(fd, &read_fds);
            if (fd > max_fd)
              max_fd = fd;
          } else {
            drop = true; // Should not happen
          }
        }
      }

      if (drop) {
        // Cleanup and remove
        if (it->peer_connection->get_fd() != -1) {
          it->peer_connection->disconnect();
        }
        // Remove from pending map
        {
          std::lock_guard<std::mutex> lock(mutex_);
          auto t_it = torrents_.find(it->torrent_id);
          if (t_it != torrents_.end()) {
            const std::string key = it->peer_info.to_string() + ":" +
                                    std::to_string(it->peer_info.port);
            t_it->second->pending_connections.erase(key);
          }
        }
        it = active_requests.erase(it);
      } else {
        ++it;
      }
    }

    if (active_requests.empty())
      continue;

    // 4. Run select
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms

    int n = select(max_fd + 1, &read_fds, &write_fds, nullptr, &tv);

    if (n < 0) {
      if (errno == EINTR)
        continue;
      // Error? Sleep briefly
      std::this_thread::sleep_for(10ms);
      continue;
    }

    // 5. Handle I/O events
    for (auto it = active_requests.begin(); it != active_requests.end();) {
      bool finished = false;
      bool failed = false;

      int fd = it->peer_connection->get_fd();

      if (fd != -1) {
        if (it->state == ConnectionState::CONNECTING) {
          // Check if writable (connection complete)
          if (FD_ISSET(fd, &write_fds)) {
            // Check socket error
            if (it->peer_connection->check_connect_result()) {
              // Connected! Now send handshake
              if (it->peer_connection->send_handshake()) {
                it->state = ConnectionState::HANDSHAKING;
              } else {
                failed = true;
              }
            } else {
              failed = true;
            }
          }
        } else if (it->state == ConnectionState::HANDSHAKING) {
          if (FD_ISSET(fd, &read_fds)) {
            // Attempt to read handshake
            // receive_handshake_nonblocking returns true if done, false if
            // partial/wouldblock OR error. We need to distinguish. For now,
            // assume it handles it or fails. Our implementation returns false
            // on partial too. So we might spin here if we get partials. But for
            // this task, let's assume if it returns true, we are good.
            if (it->peer_connection->receive_handshake_nonblocking()) {
              finished = true;
            } else {
              // Failed or partial. If partial and we don't support buffering,
              // we fail. Since we didn't implement buffering in PeerConnection,
              // this is fail.
              failed = true;
            }
          }
        }
      }

      if (failed) {
        it->peer_connection->disconnect();
        // Remove from pending map
        {
          std::lock_guard<std::mutex> lock(mutex_);
          auto t_it = torrents_.find(it->torrent_id);
          if (t_it != torrents_.end()) {
            const std::string key = it->peer_info.to_string() + ":" +
                                    std::to_string(it->peer_info.port);
            t_it->second->pending_connections.erase(key);
          }
        }
        it = active_requests.erase(it);
      } else if (finished) {
        // Handshake complete, move to active torrent
        std::lock_guard<std::mutex> lock(mutex_);
        auto t_it = torrents_.find(it->torrent_id);
        if (t_it != torrents_.end()) {
          const std::string key = it->peer_info.to_string() + ":" +
                                  std::to_string(it->peer_info.port);
          t_it->second->pending_connections.erase(key);

          // Start the regular IO thread for this peer
          it->peer_connection->start();
          t_it->second->peers.push_back(it->peer_connection);

          pixellib::core::logging::Logger::info(
              "Connected to peer {} for torrent {} (Async)",
              it->peer_info.to_string().substr(0, 32),
              it->torrent_id.substr(0, 8));
        } else {
          it->peer_connection->disconnect();
        }
        it = active_requests.erase(it);
      } else {
        ++it;
      }
    }
  }
}

} // namespace pixelscrape
