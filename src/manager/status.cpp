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
  // Copy all needed data while holding the lock
  std::string name;
  size_t total_length;
  size_t downloaded_bytes;
  size_t uploaded_bytes;
  size_t download_speed;
  size_t upload_speed;
  double completion;
  bool paused;
  size_t peers_count;
  size_t total_peers_count;
  std::vector<std::shared_ptr<PeerConnection>> peers_copy;
  std::vector<TorrentFile> files;
  std::vector<size_t> file_priorities;
  PieceManager *piece_manager_ptr = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = torrents_.find(torrent_id);
    if (it == torrents_.end()) {
      return std::nullopt;
    }

    const auto &torrent = it->second;

    // Copy all data we need (but DON'T call methods that acquire locks!)
    name = torrent->metadata.name;
    total_length = torrent->metadata.total_length;
    uploaded_bytes = torrent->uploaded_bytes;
    download_speed = torrent->download_speed;
    upload_speed = torrent->upload_speed;
    paused = torrent->paused;
    peers_count = torrent->peers.size();
    total_peers_count =
        torrent->peers.size() + torrent->discovered_peers.size();
    peers_copy = torrent->peers; // Copy shared_ptrs
    files = torrent->metadata.files;
    file_priorities = torrent->file_priorities;
    piece_manager_ptr = torrent->piece_manager.get(); // Just get the pointer
  }
  // Lock is released here

  // Now call piece manager methods WITHOUT holding the manager lock
  downloaded_bytes = piece_manager_ptr->get_total_downloaded_bytes();
  completion = piece_manager_ptr->get_completion_percentage();

  // Now build the JSON without holding the lock
  pixellib::core::json::JSON status = pixellib::core::json::JSON::object({});

  status["name"] = pixellib::core::json::JSON(name);
  status["info_hash"] = pixellib::core::json::JSON(torrent_id);
  status["total_size"] =
      pixellib::core::json::JSON(static_cast<double>(total_length));
  status["downloaded"] =
      pixellib::core::json::JSON(static_cast<double>(downloaded_bytes));
  status["uploaded"] =
      pixellib::core::json::JSON(static_cast<double>(uploaded_bytes));
  status["download_speed"] =
      pixellib::core::json::JSON(static_cast<double>(download_speed));
  status["upload_speed"] =
      pixellib::core::json::JSON(static_cast<double>(upload_speed));
  status["completion"] = pixellib::core::json::JSON(completion);
  status["paused"] = pixellib::core::json::JSON(paused);
  status["peers"] =
      pixellib::core::json::JSON(static_cast<double>(peers_count));
  status["total_peers"] =
      pixellib::core::json::JSON(static_cast<double>(total_peers_count));

  // Skip detailed peer list to avoid deadlock
  // (peer threads may call back into torrent manager via piece_callback)
  status["peers_list"] = pixellib::core::json::JSON::array({});

  // File list
  pixellib::core::json::JSON files_json = pixellib::core::json::JSON::array({});
  for (size_t i = 0; i < files.size(); ++i) {
    const auto &file = files[i];
    pixellib::core::json::JSON file_obj =
        pixellib::core::json::JSON::object({});
    file_obj["path"] = pixellib::core::json::JSON(file.path);
    file_obj["size"] =
        pixellib::core::json::JSON(static_cast<double>(file.length));
    file_obj["priority"] =
        pixellib::core::json::JSON(static_cast<double>(file_priorities[i]));
    files_json.push_back(file_obj);
  }
  status["files"] = files_json;

  return status;
}

void TorrentManager::update_speeds() {
  // Collect torrent IDs and piece manager pointers without holding lock for
  // long
  struct UpdateData {
    std::string id;
    PieceManager *piece_manager;
    size_t uploaded_bytes;
    std::chrono::steady_clock::time_point last_update;
    size_t last_downloaded;
    size_t last_uploaded;
  };

  std::vector<UpdateData> update_list;
  auto now = std::chrono::steady_clock::now();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &pair : torrents_) {
      auto &torrent = pair.second;
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - torrent->last_speed_update);

      if (duration.count() >= 500) {
        UpdateData data;
        data.id = pair.first;
        data.piece_manager = torrent->piece_manager.get();
        data.uploaded_bytes = torrent->uploaded_bytes;
        data.last_update = torrent->last_speed_update;
        data.last_downloaded = torrent->last_downloaded_bytes;
        data.last_uploaded = torrent->last_uploaded_bytes;
        update_list.push_back(data);
      }
    }
  }
  // Lock released

  // Query piece managers without holding lock
  for (auto &data : update_list) {
    size_t downloaded = data.piece_manager->get_total_downloaded_bytes();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - data.last_update);

    size_t diff_down = (downloaded >= data.last_downloaded)
                           ? (downloaded - data.last_downloaded)
                           : 0;
    size_t diff_up = (data.uploaded_bytes >= data.last_uploaded)
                         ? (data.uploaded_bytes - data.last_uploaded)
                         : 0;

    size_t down_speed =
        static_cast<size_t>(diff_down * 1000.0 / duration.count());
    size_t up_speed = static_cast<size_t>(diff_up * 1000.0 / duration.count());

    // Now update the torrent with new values
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = torrents_.find(data.id);
    if (it != torrents_.end()) {
      it->second->download_speed = down_speed;
      it->second->upload_speed = up_speed;
      it->second->last_downloaded_bytes = downloaded;
      it->second->last_uploaded_bytes = data.uploaded_bytes;
      it->second->last_speed_update = now;
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
