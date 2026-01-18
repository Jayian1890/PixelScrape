#include "manager.hpp"

namespace pixelscrape {

TorrentManager::TorrentManager(const std::string &download_dir,
                               const std::string &state_dir)
    : download_dir_(download_dir), state_manager_(state_dir) {

  // Initialize DHT client
  dht_client_ = std::make_unique<dht::DHTClient>(6881);
  dht_client_->start(state_dir);

  // Start connection worker (handles blocking connect/handshake off the
  // main peer_worker thread)
  connection_worker_running_ = true;
  connection_thread_ = std::thread(&TorrentManager::connection_worker, this);

  // Start incoming TCP listener (accepts peer connections on the
  // BitTorrent port). If bind fails, we log and continue (outgoing still
  // works).
  tcp_listener_running_ = true;
  tcp_listener_thread_ =
      std::thread(&TorrentManager::tcp_listener, this, (uint16_t)6881);

  // Restore previously saved torrents
  restore_torrents();
}

TorrentManager::~TorrentManager() {
  if (dht_client_) {
    dht_client_->stop();
  }

  // Stop connection worker
  connection_worker_running_ = false;
  connection_cv_.notify_all();
  if (connection_thread_.joinable()) {
    connection_thread_.join();
  }

  // Stop TCP listener
  tcp_listener_running_ = false;
  if (tcp_listener_thread_.joinable()) {
    tcp_listener_thread_.join();
  }

  std::vector<std::unique_ptr<Torrent>> torrents_to_shutdown;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &pair : torrents_) {
      // Signal stop inside the lock
      {
        std::lock_guard<std::mutex> t_lock(pair.second->mutex);
        pair.second->stopping = true;
        pair.second->cv.notify_all();

        // Save state - load existing to preserve torrent_data
        auto existing_state = state_manager_.load_state(pair.first);
        TorrentState state;
        state.info_hash_hex = pair.first;
        state.torrent_data = existing_state ? existing_state->torrent_data : "";
        state.bitfield = pair.second->piece_manager->get_bitfield();
        state.uploaded_bytes = pair.second->uploaded_bytes;
        state.downloaded_bytes = pair.second->downloaded_bytes;
        state.file_priorities = pair.second->file_priorities;
        state.last_updated = std::chrono::system_clock::now();

        state_manager_.save_state(pair.first, state);
      }
      torrents_to_shutdown.push_back(std::move(pair.second));
    }
    torrents_.clear();
  }

  // Join threads outside the global lock
  for (auto &torrent : torrents_to_shutdown) {
    if (torrent->tracker_thread.joinable()) {
      torrent->tracker_thread.join();
    }
    if (torrent->peer_thread.joinable()) {
      torrent->peer_thread.join();
    }
  }
}

void TorrentManager::restore_torrents() {
  auto saved_torrents = state_manager_.list_saved_torrents();

  for (const auto &info_hash_hex : saved_torrents) {
    try {
      auto state = state_manager_.load_state(info_hash_hex);
      if (!state || state->torrent_data.empty()) {
        // No torrent data saved (e.g., magnet link), skip restoration
        pixellib::core::logging::Logger::debug(
            "Skipping restoration of {} - no torrent data",
            info_hash_hex.substr(0, 8));
        continue;
      }

      auto metadata = TorrentMetadataParser::parse(state->torrent_data);
      add_torrent_impl(std::move(metadata), state->file_priorities,
                       state->torrent_data);

      pixellib::core::logging::Logger::info("Restored torrent: {}",
                                            info_hash_hex.substr(0, 8));
    } catch (const std::exception &e) {
      pixellib::core::logging::Logger::warning(
          "Failed to restore torrent {}: {}", info_hash_hex.substr(0, 8),
          e.what());
    }
  }

  if (!saved_torrents.empty()) {
    pixellib::core::logging::Logger::info(
        "Torrent restoration complete ({} found)", saved_torrents.size());
  }
}

} // namespace pixelscrape
