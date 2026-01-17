#include "manager.hpp"

namespace pixelscrape {

void TorrentManager::peer_worker(const std::string &torrent_id) {
  while (true) {
    std::shared_ptr<Torrent> torrent_shared; // Hold shared ownership if needed,
                                             // but here we access via map
    Torrent *torrent = nullptr;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      auto it = torrents_.find(torrent_id);
      if (it == torrents_.end())
        break;

      torrent = it->second.get();
    }

    // Check stopping condition safely
    {
      std::lock_guard<std::mutex> t_lock(torrent->mutex);
      if (torrent->stopping)
        break;
    }

    if (torrent->paused) {
      std::unique_lock<std::mutex> t_lock(torrent->mutex);
      if (torrent->cv.wait_for(t_lock, std::chrono::seconds(1),
                               [&]() { return torrent->stopping; }))
        break;
      continue;
    }

    // 1. Manage Peers
    {
      // Copy discovered peers to local list to iterate without lock
      std::vector<PeerInfo> discovered_copy;
      {
        std::lock_guard<std::mutex> t_lock(torrent->mutex);
        discovered_copy = torrent->discovered_peers;
      }

      // Copy existing peers to local list
      std::vector<std::shared_ptr<PeerConnection>> peers_copy;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = torrents_.find(torrent_id);
        if (it != torrents_.end()) {
          peers_copy = it->second->peers;
        }
      }

      // Connect to new peers by enqueueing connection requests (non-blocking)
      for (const auto &peer_info : discovered_copy) {
        if (peers_copy.size() >= 50)
          break;

        // Create new connection object; connection handshake will be handled
        // by the connection worker
        auto peer_conn = std::make_shared<PeerConnection>(
            torrent->metadata, torrent->metadata.info_hash, torrent->peer_id,
            peer_info, *torrent->piece_manager);

        // Set callbacks
        peer_conn->set_piece_callback([this, torrent_id](
                                          size_t index, size_t begin,
                                          const std::vector<uint8_t> &data) {
          std::lock_guard<std::mutex> lock(mutex_);
          auto it = torrents_.find(torrent_id);
          if (it != torrents_.end()) {
            if (it->second->piece_manager->receive_block(index, begin, data)) {
              it->second->downloaded_bytes += data.size();
              if (it->second->piece_manager->is_piece_complete(index)) {
                if (it->second->piece_manager->verify_piece(index)) {
                  // Piece verified!
                  // Send HAVE to all peers
                  // Use a copy to avoid iterating while modified or locking
                  // issues
                  auto peers_snapshot = it->second->peers;
                  for (auto &p : peers_snapshot) {
                    // Sending HAVE is async/non-blocking ideally, or fast
                    // enough
                    if (p->is_connected()) {
                      p->set_have_piece(index, true);
                    }
                  }
                }
              }
            }
          }
        });

        // Enqueue connection request — do not block the peer_worker thread.
        {
          std::lock_guard<std::mutex> t_lock(torrent->mutex);
          const std::string peer_key =
              peer_info.to_string() + ":" + std::to_string(peer_info.port);
          if (torrent->pending_connections.find(peer_key) ==
              torrent->pending_connections.end()) {
            torrent->pending_connections.insert(peer_key);
            ConnectionRequest req;
            req.torrent_id = torrent_id;
            req.peer_info = peer_info;
            req.peer_connection = peer_conn;
            req.state = ConnectionState::CONNECTING;
            req.start_time = std::chrono::steady_clock::now();

            {
              std::lock_guard<std::mutex> lock(connection_mutex_);
              connection_queue_.push(std::move(req));
            }
            connection_cv_.notify_one();
          }
        }
      }

      // Cleanup disconnected peers (regular maintenance)
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = torrents_.find(torrent_id);
        if (it != torrents_.end()) {
          it->second->peers.erase(
              std::remove_if(it->second->peers.begin(), it->second->peers.end(),
                             [](const std::shared_ptr<PeerConnection> &p) {
                               return !p->is_connected();
                             }),
              it->second->peers.end());
        }
      }
    }

    // 2. Download Logic
    {
      // We need to lock to access piece manager and peer state safely
      std::unique_lock<std::mutex> lock(mutex_);
      auto it = torrents_.find(torrent_id);
      if (it != torrents_.end()) {
        torrent = it->second.get();

        // If we have completed the torrent, we are seeding
        if (torrent->piece_manager->get_completion_percentage() >= 100.0) {
          // Seeding logic (uploading is handled by PeerConnection responding to
          // requests)
        } else {
          // Downloading
          std::vector<std::vector<bool>> peer_bitfields;
          for (const auto &peer : torrent->peers) {
            if (peer->is_connected()) {
              peer_bitfields.push_back(peer->get_bitfield());
            }
          }

          size_t rarest_piece =
              torrent->piece_manager->select_rarest_piece(peer_bitfields);

          if (rarest_piece != SIZE_MAX) {
            // Find peers that have this piece and are unchoked
            const size_t block_size = 16384; // 16KB blocks

            // Calculate actual piece size for the last piece
            size_t piece_size =
                (rarest_piece == torrent->metadata.piece_hashes.size() - 1)
                    ? (torrent->metadata.total_length %
                       torrent->metadata.piece_length)
                    : torrent->metadata.piece_length;
            if (piece_size == 0)
              piece_size = torrent->metadata.piece_length;

            size_t num_blocks = (piece_size + block_size - 1) / block_size;

            // Try to request blocks from available peers
            for (auto &peer : torrent->peers) {
              if (!peer->is_connected())
                continue;

              const auto &bitfield = peer->get_bitfield();
              if (rarest_piece >= bitfield.size() || !bitfield[rarest_piece]) {
                continue; // Peer doesn't have this piece
              }

              if (peer->is_choking()) {
                // Send interested if not already
                peer->set_interested(true);
                continue;
              }

              // Request up to 4 blocks per iteration from this peer
              int blocks_requested = 0;
              for (size_t block_idx = 0;
                   block_idx < num_blocks && blocks_requested < 4;
                   ++block_idx) {
                size_t begin = block_idx * block_size;
                size_t length = std::min(block_size, piece_size - begin);

                if (torrent->piece_manager->request_block(
                        rarest_piece, block_idx, block_size)) {
                  peer->send_request(rarest_piece, begin, length);
                  blocks_requested++;
                }
              }

              if (blocks_requested > 0) {
                break; // Move to next loop iteration, try other pieces
              }
            }
          }
        }
      }
    }

    // Sleep a bit to avoid busy loop
    {
      std::unique_lock<std::mutex> t_lock(torrent->mutex);
      if (torrent->cv.wait_for(t_lock, std::chrono::milliseconds(100),
                               [&]() { return torrent->stopping; }))
        break;
    }
  }
}

} // namespace pixelscrape
