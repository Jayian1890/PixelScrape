#include "manager.hpp"

namespace pixelscrape {

namespace {

// Internal struct to hold copied data for status generation
struct TorrentStatusData {
    std::string id;
    std::string name;
    size_t total_length;
    size_t uploaded_bytes;
    size_t download_speed;
    size_t upload_speed;
    bool paused;
    size_t peers_count;
    size_t total_peers_count;
    std::vector<TorrentFile> files;
    std::vector<size_t> file_priorities;
    PieceManager* piece_manager_ptr;
};

// Helper to build JSON from status data
pixellib::core::json::JSON build_status_json(const TorrentStatusData& data) {
    size_t downloaded_bytes = data.piece_manager_ptr->get_total_downloaded_bytes();
    double completion = data.piece_manager_ptr->get_completion_percentage();

    pixellib::core::json::JSON status = pixellib::core::json::JSON::object({});

    status["name"] = pixellib::core::json::JSON(data.name);
    status["info_hash"] = pixellib::core::json::JSON(data.id);
    status["total_size"] = pixellib::core::json::JSON(static_cast<double>(data.total_length));
    status["downloaded"] = pixellib::core::json::JSON(static_cast<double>(downloaded_bytes));
    status["uploaded"] = pixellib::core::json::JSON(static_cast<double>(data.uploaded_bytes));
    status["download_speed"] = pixellib::core::json::JSON(static_cast<double>(data.download_speed));
    status["upload_speed"] = pixellib::core::json::JSON(static_cast<double>(data.upload_speed));
    status["completion"] = pixellib::core::json::JSON(completion);
    status["paused"] = pixellib::core::json::JSON(data.paused);
    status["peers"] = pixellib::core::json::JSON(static_cast<double>(data.peers_count));
    status["total_peers"] = pixellib::core::json::JSON(static_cast<double>(data.total_peers_count));
    status["peers_list"] = pixellib::core::json::JSON::array({});

    pixellib::core::json::JSON files_json = pixellib::core::json::JSON::array({});
    for (size_t i = 0; i < data.files.size(); ++i) {
        const auto &file = data.files[i];
        pixellib::core::json::JSON file_obj = pixellib::core::json::JSON::object({});
        file_obj["path"] = pixellib::core::json::JSON(file.path);
        file_obj["size"] = pixellib::core::json::JSON(static_cast<double>(file.length));
        file_obj["priority"] = pixellib::core::json::JSON(static_cast<double>(data.file_priorities[i]));
        files_json.push_back(file_obj);
    }
    status["files"] = files_json;

    return status;
}

} // namespace

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
  TorrentStatusData data;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = torrents_.find(torrent_id);
    if (it == torrents_.end()) {
      return std::nullopt;
    }

    const auto &torrent = it->second;
    data.id = torrent_id;
    data.name = torrent->metadata.name;
    data.total_length = torrent->metadata.total_length;
    data.uploaded_bytes = torrent->uploaded_bytes;
    data.download_speed = torrent->download_speed;
    data.upload_speed = torrent->upload_speed;
    data.paused = torrent->paused;
    data.peers_count = torrent->peers.size();
    data.total_peers_count = torrent->peers.size() + torrent->discovered_peers.size();
    data.files = torrent->metadata.files;
    data.file_priorities = torrent->file_priorities;
    data.piece_manager_ptr = torrent->piece_manager.get();
  }

  return build_status_json(data);
}

std::vector<pixellib::core::json::JSON>
TorrentManager::get_torrents_status(const std::vector<std::string> &torrent_ids) const {
  std::vector<TorrentStatusData> data_list;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (torrent_ids.empty()) {
        // Retrieve all torrents
        data_list.reserve(torrents_.size());
        for (const auto &pair : torrents_) {
            const auto &id = pair.first;
            const auto &torrent = pair.second;

            TorrentStatusData data;
            data.id = id;
            data.name = torrent->metadata.name;
            data.total_length = torrent->metadata.total_length;
            data.uploaded_bytes = torrent->uploaded_bytes;
            data.download_speed = torrent->download_speed;
            data.upload_speed = torrent->upload_speed;
            data.paused = torrent->paused;
            data.peers_count = torrent->peers.size();
            data.total_peers_count = torrent->peers.size() + torrent->discovered_peers.size();
            data.files = torrent->metadata.files;
            data.file_priorities = torrent->file_priorities;
            data.piece_manager_ptr = torrent->piece_manager.get();

            data_list.push_back(std::move(data));
        }
    } else {
        // Retrieve specific torrents
        data_list.reserve(torrent_ids.size());
        for (const auto &id : torrent_ids) {
            auto it = torrents_.find(id);
            if (it != torrents_.end()) {
                const auto &torrent = it->second;

                TorrentStatusData data;
                data.id = id;
                data.name = torrent->metadata.name;
                data.total_length = torrent->metadata.total_length;
                data.uploaded_bytes = torrent->uploaded_bytes;
                data.download_speed = torrent->download_speed;
                data.upload_speed = torrent->upload_speed;
                data.paused = torrent->paused;
                data.peers_count = torrent->peers.size();
                data.total_peers_count = torrent->peers.size() + torrent->discovered_peers.size();
                data.files = torrent->metadata.files;
                data.file_priorities = torrent->file_priorities;
                data.piece_manager_ptr = torrent->piece_manager.get();

                data_list.push_back(std::move(data));
            }
        }
    }
  }

  std::vector<pixellib::core::json::JSON> result;
  result.reserve(data_list.size());
  for (const auto &data : data_list) {
      result.push_back(build_status_json(data));
  }

  return result;
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
