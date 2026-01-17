#include "manager.hpp"

namespace pixelscrape {

void TorrentManager::tracker_worker(const std::string &torrent_id) {
  bool first_run = true;

  while (true) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = torrents_.find(torrent_id);
    if (it == torrents_.end()) {
      break; // Torrent removed
    }

    // Check stopping condition safely
    {
      std::lock_guard<std::mutex> t_lock(it->second->mutex);
      if (it->second->stopping)
        break;
    }

    auto &torrent = it->second;
    if (torrent->paused) {
      lock.unlock();
      std::unique_lock<std::mutex> t_lock(torrent->mutex);
      if (torrent->cv.wait_for(t_lock, std::chrono::seconds(5),
                               [&]() { return torrent->stopping; }))
        break;
      continue;
    }

    // Get the list of all tracker URLs to query
    std::vector<std::string> tracker_urls = torrent->metadata.announce_list;

    // Unlock main mutex before network operations
    lock.unlock();

    if (first_run) {
      pixellib::core::logging::Logger::info(
          "Connecting to {} tracker(s) for torrent {}", tracker_urls.size(),
          torrent_id.substr(0, 8));
    }

    // Query ALL trackers in the announce_list
    size_t total_new_peers = 0;
    for (const auto &tracker_url : tracker_urls) {
      // Check if we should stop before each tracker request
      {
        std::lock_guard<std::mutex> t_lock(torrent->mutex);
        if (torrent->stopping)
          break;
      }

      try {
        auto peers = torrent->tracker->get_peers(
            torrent->peer_id,
            6881, // Default port
            torrent->uploaded_bytes, torrent->downloaded_bytes,
            torrent->metadata.total_length - torrent->downloaded_bytes,
            first_run ? "started"
                      : "", // Send "started" event on first announce
            tracker_url     // Query this specific tracker
        );

        // Re-lock to update shared data
        std::lock_guard<std::mutex> t_lock(torrent->mutex);
        for (const auto &peer_info : peers) {
          bool known = false;
          for (const auto &existing : torrent->discovered_peers) {
            if (existing.ip == peer_info.ip &&
                existing.port == peer_info.port) {
              known = true;
              break;
            }
          }
          if (!known) {
            torrent->discovered_peers.push_back(peer_info);
            total_new_peers++;
          }
        }

      } catch (const std::exception &e) {
        pixellib::core::logging::Logger::warning(
            "Tracker {} error for {}: {}", tracker_url.substr(0, 50),
            torrent_id.substr(0, 8), e.what());
      }
    }

    if (total_new_peers > 0 || first_run) {
      std::lock_guard<std::mutex> t_lock(torrent->mutex);
      pixellib::core::logging::Logger::info(
          "Discovered {} new peer(s) for torrent {} (total: {})",
          total_new_peers, torrent_id.substr(0, 8),
          torrent->discovered_peers.size());
    }

    first_run = false;

    // Wait for next update or stop signal (5 minutes)
    Torrent *torrent_ptr = torrent.get();

    std::unique_lock<std::mutex> t_lock(torrent_ptr->mutex);
    if (torrent_ptr->cv.wait_for(t_lock, std::chrono::minutes(5),
                                 [&]() { return torrent_ptr->stopping; })) {
      break;
    }
  }
}

} // namespace pixelscrape
