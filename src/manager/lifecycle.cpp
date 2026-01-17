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

        // Save state
        TorrentState state;
        state.info_hash_hex = pair.first;
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

} // namespace pixelscrape
