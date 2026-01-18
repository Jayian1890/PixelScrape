#include "manager.hpp"

namespace pixelscrape {

// TCP listener: accept incoming peer connections, validate the BitTorrent
// handshake, and attach valid peers to the matching torrent.
void TorrentManager::tcp_listener(uint16_t port) {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    pixellib::core::logging::Logger::warning(
        "tcp_listener: socket() failed: {}", strerror(errno));
    return;
  }

  int one = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    pixellib::core::logging::Logger::warning(
        "tcp_listener: bind({}) failed: {}", port, strerror(errno));
    close(server_fd);
    return;
  }

  if (listen(server_fd, 128) < 0) {
    pixellib::core::logging::Logger::warning("tcp_listener: listen failed: {}",
                                             strerror(errno));
    close(server_fd);
    return;
  }

  // Make non-blocking so we can check running flag periodically
  int flags = fcntl(server_fd, F_GETFL, 0);
  fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

  pixellib::core::logging::Logger::info("tcp_listener: accepting peers on {}",
                                        port);

  while (tcp_listener_running_) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(
        server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      } else {
        pixellib::core::logging::Logger::warning(
            "tcp_listener: accept error: {}", strerror(errno));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
    }

    // Set a short recv timeout for the handshake
    struct timeval tv{3, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Read handshake (68 bytes expected)
    uint8_t buf[68];
    ssize_t r = recv(client_fd, buf, sizeof(buf), MSG_WAITALL);
    if (r != static_cast<ssize_t>(sizeof(buf))) {
      close(client_fd);
      continue;
    }

    // Parse handshake
    if (buf[0] != 19) {
      close(client_fd);
      continue;
    }
    const char expected_proto[] = "BitTorrent protocol";
    if (std::memcmp(buf + 1, expected_proto, 19) != 0) {
      close(client_fd);
      continue;
    }

    std::array<uint8_t, 8> remote_reserved{};
    std::memcpy(remote_reserved.data(), buf + 20, 8);

    std::array<uint8_t, 20> remote_info_hash{};
    std::memcpy(remote_info_hash.data(), buf + 28, 20);

    std::array<uint8_t, 20> remote_peer_id{};
    std::memcpy(remote_peer_id.data(), buf + 48, 20);

    // Find matching torrent by info_hash
    std::string matched_torrent_id;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::string info_hash_str(reinterpret_cast<const char*>(remote_info_hash.data()), 20);
      auto it = info_hash_to_id_.find(info_hash_str);
      if (it != info_hash_to_id_.end()) {
        matched_torrent_id = it->second;
      }
    }

    if (matched_torrent_id.empty()) {
      // No torrent for this info_hash; politely close
      close(client_fd);
      continue;
    }

    // Build PeerInfo from sockaddr
    PeerInfo peer_info{};
    if (client_addr.sin_family == AF_INET) {
      uint32_t ip = ntohl(client_addr.sin_addr.s_addr);
      peer_info.ip = {static_cast<uint8_t>((ip >> 24) & 0xFF),
                      static_cast<uint8_t>((ip >> 16) & 0xFF),
                      static_cast<uint8_t>((ip >> 8) & 0xFF),
                      static_cast<uint8_t>(ip & 0xFF)};
      peer_info.port = ntohs(client_addr.sin_port);
    } else {
      close(client_fd);
      continue;
    }

    // Attach to torrent (dedupe checks)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = torrents_.find(matched_torrent_id);
      if (it == torrents_.end()) {
        close(client_fd);
        continue;
      }

      // Check peer cap
      if (it->second->peers.size() >= 50) {
        close(client_fd);
        continue;
      }

      const std::string peer_key =
          peer_info.to_string() + ":" + std::to_string(peer_info.port);
      if (it->second->pending_connections.find(peer_key) !=
          it->second->pending_connections.end()) {
        // Duplicate attempt
        close(client_fd);
        continue;
      }

      bool already_connected = false;
      for (const auto &pc : it->second->peers) {
        if (pc->get_peer_info().ip == peer_info.ip &&
            pc->get_peer_info().port == peer_info.port) {
          already_connected = true;
          break;
        }
      }
      if (already_connected) {
        close(client_fd);
        continue;
      }

      // Reserve the slot
      it->second->pending_connections.insert(peer_key);

      // Create PeerConnection from accepted socket
      try {
        auto peer_conn = std::make_shared<PeerConnection>(
            client_fd, it->second->metadata, it->second->metadata.info_hash,
            it->second->peer_id, peer_info, *it->second->piece_manager, true);

        // Set piece callback (same as for outgoing connections)
        std::string tid = matched_torrent_id; // copy for lambda
        peer_conn->set_piece_callback([this,
                                       tid](size_t index, size_t begin,
                                            const std::vector<uint8_t> &data) {
          std::lock_guard<std::mutex> lock(mutex_);
          auto it = torrents_.find(tid);
          if (it != torrents_.end()) {
            if (it->second->piece_manager->receive_block(index, begin, data)) {
              it->second->downloaded_bytes += data.size();
              if (it->second->piece_manager->is_piece_complete(index)) {
                if (it->second->piece_manager->verify_piece(index)) {
                  auto peers_snapshot = it->second->peers;
                  for (auto &p : peers_snapshot) {
                    if (p->is_connected()) {
                      p->set_have_piece(index, true);
                    }
                  }
                }
              }
            }
          }
        });

        // Respond to the already-received handshake and start IO loop
        if (!peer_conn->respond_to_handshake(remote_peer_id, remote_reserved)) {
          peer_conn->disconnect();
          std::lock_guard<std::mutex> l2(mutex_);
          auto it2 = torrents_.find(matched_torrent_id);
          if (it2 != torrents_.end())
            it2->second->pending_connections.erase(peer_key);
          continue;
        }

        peer_conn->start();

        // Add to active peers
        it->second->peers.push_back(peer_conn);
        it->second->pending_connections.erase(peer_key);

        pixellib::core::logging::Logger::info(
            "tcp_listener: accepted peer {} for torrent {}",
            peer_info.to_string().substr(0, 32),
            matched_torrent_id.substr(0, 8));

      } catch (const std::exception &e) {
        pixellib::core::logging::Logger::warning(
            "tcp_listener: failed to attach peer: {}", e.what());
        close(client_fd);
        it->second->pending_connections.erase(peer_key);
        continue;
      }
    }
  }

  close(server_fd);
}

} // namespace pixelscrape
