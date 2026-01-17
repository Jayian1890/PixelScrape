#include "manager.hpp"

namespace pixelscrape {

std::vector<std::string> TorrentManager::list_torrents() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> ids;
  for (const auto &pair : torrents_) {
    ids.push_back(pair.first);
  }
  return ids;
}

std::optional<pixellib::core::json::JSON>
TorrentManager::get_torrent_status(const std::string &torrent_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = torrents_.find(torrent_id);
  if (it == torrents_.end()) {
    return std::nullopt;
  }

  const auto &torrent = it->second;
  pixellib::core::json::JSON status = pixellib::core::json::JSON::object({});

  status["name"] = pixellib::core::json::JSON(torrent->metadata.name);
  status["info_hash"] = pixellib::core::json::JSON(torrent_id);
  status["total_size"] = pixellib::core::json::JSON(
      static_cast<double>(torrent->metadata.total_length));
  status["downloaded"] = pixellib::core::json::JSON(static_cast<double>(
      torrent->piece_manager->get_total_downloaded_bytes()));
  status["uploaded"] =
      pixellib::core::json::JSON(static_cast<double>(torrent->uploaded_bytes));
  status["download_speed"] =
      pixellib::core::json::JSON(static_cast<double>(torrent->download_speed));
  status["upload_speed"] =
      pixellib::core::json::JSON(static_cast<double>(torrent->upload_speed));
  status["completion"] = pixellib::core::json::JSON(
      torrent->piece_manager->get_completion_percentage());
  status["paused"] = pixellib::core::json::JSON(torrent->paused);
  status["peers"] =
      pixellib::core::json::JSON(static_cast<double>(torrent->peers.size()));
  status["total_peers"] = pixellib::core::json::JSON(static_cast<double>(
      torrent->peers.size() + torrent->discovered_peers.size()));

  // Peer list
  pixellib::core::json::JSON peers_list = pixellib::core::json::JSON::array({});
  for (const auto &pc : torrent->peers) {
    if (pc->is_connected()) {
      pixellib::core::json::JSON peer_obj =
          pixellib::core::json::JSON::object({});
      const auto &info = pc->get_peer_info();
      peer_obj["ip"] = pixellib::core::json::JSON(info.to_string());
      peer_obj["port"] =
          pixellib::core::json::JSON(static_cast<double>(info.port));
      peer_obj["client"] = pixellib::core::json::JSON("BitTorrent Client");
      peer_obj["choking"] = pixellib::core::json::JSON(pc->is_choking());
      peers_list.push_back(peer_obj);
    }
  }
  status["peers_list"] = peers_list;

  // File list
  pixellib::core::json::JSON files = pixellib::core::json::JSON::array({});
  for (size_t i = 0; i < torrent->metadata.files.size(); ++i) {
    const auto &file = torrent->metadata.files[i];
    pixellib::core::json::JSON file_obj =
        pixellib::core::json::JSON::object({});
    file_obj["path"] = pixellib::core::json::JSON(file.path);
    file_obj["size"] =
        pixellib::core::json::JSON(static_cast<double>(file.length));
    file_obj["priority"] = pixellib::core::json::JSON(
        static_cast<double>(torrent->file_priorities[i]));
    files.push_back(file_obj);
  }
  status["files"] = files;

  return status;
}

void TorrentManager::update_speeds() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();

  for (auto &pair : torrents_) {
    auto &torrent = pair.second;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - torrent->last_speed_update);

    if (duration.count() >= 500) { // Update every 0.5s or more
      size_t downloaded = torrent->piece_manager->get_total_downloaded_bytes();
      size_t uploaded = torrent->uploaded_bytes;

      size_t diff_down = (downloaded >= torrent->last_downloaded_bytes)
                             ? (downloaded - torrent->last_downloaded_bytes)
                             : 0;
      size_t diff_up = (uploaded >= torrent->last_uploaded_bytes)
                           ? (uploaded - torrent->last_uploaded_bytes)
                           : 0;

      torrent->download_speed =
          static_cast<size_t>(diff_down * 1000.0 / duration.count());
      torrent->upload_speed =
          static_cast<size_t>(diff_up * 1000.0 / duration.count());

      torrent->last_downloaded_bytes = downloaded;
      torrent->last_uploaded_bytes = uploaded;
      torrent->last_speed_update = now;
    }
  }
}

pixellib::core::json::JSON TorrentManager::get_global_stats() const {
  std::lock_guard<std::mutex> lock(mutex_);

  pixellib::core::json::JSON stats = pixellib::core::json::JSON::object({});
  stats["total_torrents"] =
      pixellib::core::json::JSON(static_cast<double>(torrents_.size()));

  size_t total_downloaded = 0;
  size_t total_uploaded = 0;
  size_t active_peers = 0;
  size_t total_down_speed = 0;
  size_t total_up_speed = 0;

  for (const auto &pair : torrents_) {
    total_downloaded +=
        pair.second->piece_manager->get_total_downloaded_bytes();
    total_uploaded += pair.second->uploaded_bytes;
    active_peers += pair.second->peers.size();
    total_down_speed += pair.second->download_speed;
    total_up_speed += pair.second->upload_speed;
  }

  stats["total_downloaded"] =
      pixellib::core::json::JSON(static_cast<double>(total_downloaded));
  stats["total_uploaded"] =
      pixellib::core::json::JSON(static_cast<double>(total_uploaded));
  stats["active_peers"] =
      pixellib::core::json::JSON(static_cast<double>(active_peers));
  stats["download_speed"] =
      pixellib::core::json::JSON(static_cast<double>(total_down_speed));
  stats["upload_speed"] =
      pixellib::core::json::JSON(static_cast<double>(total_up_speed));

  if (dht_client_) {
    stats["dht_active"] = pixellib::core::json::JSON(dht_client_->is_running());
    stats["dht_nodes"] = pixellib::core::json::JSON(
        static_cast<double>(dht_client_->node_count()));
    stats["dht_queries"] = pixellib::core::json::JSON(
        static_cast<double>(dht_client_->active_queries()));
  }

  return stats;
}

} // namespace pixelscrape
