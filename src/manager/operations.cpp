#include "manager.hpp"

namespace pixelscrape {

std::string
TorrentManager::add_torrent(const std::string &torrent_path,
                            const std::vector<size_t> &file_priorities) {
  std::string data =
      pixellib::core::filesystem::FileSystem::read_file(torrent_path);
  auto metadata = TorrentMetadataParser::parse(data);
  return add_torrent_impl(std::move(metadata), file_priorities, data);
}

std::string
TorrentManager::add_torrent_data(const std::string &data,
                                 const std::vector<size_t> &file_priorities) {
  auto metadata = TorrentMetadataParser::parse(data);
  return add_torrent_impl(std::move(metadata), file_priorities, data);
}

std::string TorrentManager::add_magnet_link(const std::string &magnet_uri) {
  // Parse magnet link
  if (magnet_uri.find("magnet:?") != 0) {
    throw std::runtime_error("Invalid magnet link format");
  }

  size_t xt_pos = magnet_uri.find("xt=urn:btih:");
  if (xt_pos == std::string::npos) {
    throw std::runtime_error("Magnet link missing info hash");
  }

  size_t hash_start = xt_pos + 12;
  size_t hash_end = magnet_uri.find('&', hash_start);
  if (hash_end == std::string::npos)
    hash_end = magnet_uri.size();
  std::string hash_hex = magnet_uri.substr(hash_start, hash_end - hash_start);

  if (hash_hex.size() != 40) {
    throw std::runtime_error("Invalid info hash length");
  }

  std::array<uint8_t, 20> info_hash;
  for (size_t i = 0; i < 20; ++i) {
    std::string byte_str = hash_hex.substr(i * 2, 2);
    info_hash[i] = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
  }

  // Create torrent ID from info hash
  std::ostringstream oss;
  for (uint8_t byte : info_hash) {
    oss << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(byte);
  }
  std::string torrent_id = oss.str();

  pixellib::core::logging::Logger::info("Adding magnet link with info hash: {}",
                                        torrent_id);

  // Start DHT peer discovery
  if (dht_client_ && dht_client_->is_running()) {
    dht_client_->find_peers(
        info_hash,
        [this, torrent_id](const std::array<uint8_t, 20> & /*hash*/,
                           const std::vector<dht::TorrentPeer> &peers) {
          std::lock_guard<std::mutex> lock(mutex_);
          auto it = torrents_.find(torrent_id);
          if (it == torrents_.end()) {
            return; // Torrent was removed
          }

          auto &torrent = it->second;
          std::lock_guard<std::mutex> t_lock(torrent->mutex);

          size_t added = 0;
          for (const auto &dht_peer : peers) {
            // Check if peer already known
            bool known = false;
            for (const auto &existing : torrent->discovered_peers) {
              if (existing.ip == dht_peer.ip &&
                  existing.port == dht_peer.port) {
                known = true;
                break;
              }
            }
            if (!known) {
              PeerInfo peer_info;
              peer_info.ip = dht_peer.ip;
              peer_info.port = dht_peer.port;
              torrent->discovered_peers.push_back(peer_info);
              added++;
            }
          }

          if (added > 0) {
            pixellib::core::logging::Logger::info(
                "DHT: Added {} new peer(s) for torrent {} (total: {})", added,
                torrent_id.substr(0, 8), torrent->discovered_peers.size());
          }
        });
  }

  // Return torrent ID (metadata will be fetched asynchronously)
  return torrent_id;
}

std::string
TorrentManager::add_torrent_impl(TorrentMetadata metadata,
                                 const std::vector<size_t> &file_priorities) {
  // Delegate to overload with empty torrent data (for magnet links)
  return add_torrent_impl(std::move(metadata), file_priorities, "");
}

std::string
TorrentManager::add_torrent_impl(TorrentMetadata metadata,
                                 const std::vector<size_t> &file_priorities,
                                 const std::string &raw_torrent_data) {
  std::string info_hash_hex =
      StateManager::info_hash_to_hex(metadata.info_hash);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (torrents_.find(info_hash_hex) != torrents_.end()) {
      throw std::runtime_error("Torrent already exists");
    }
  }

  auto torrent = std::make_unique<Torrent>();
  torrent->metadata = std::move(metadata);
  torrent->peer_id = generate_peer_id();
  torrent->tracker = std::make_unique<TrackerClient>(torrent->metadata);

  // This part involves disk IO and can be slow
  torrent->piece_manager = std::make_unique<PieceManager>(
      torrent->metadata, download_dir_, file_priorities);

  if (file_priorities.empty()) {
    torrent->file_priorities.assign(torrent->metadata.files.size(), 1);
  } else {
    torrent->file_priorities = file_priorities;
    torrent->file_priorities.resize(torrent->metadata.files.size(), 0);
  }

  auto saved_state = state_manager_.load_state(info_hash_hex);
  if (saved_state) {
    torrent->piece_manager->load_state(saved_state->bitfield);
    torrent->uploaded_bytes = saved_state->uploaded_bytes;
    torrent->downloaded_bytes = saved_state->downloaded_bytes;
    if (!saved_state->file_priorities.empty()) {
      torrent->file_priorities = saved_state->file_priorities;
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (torrents_.find(info_hash_hex) != torrents_.end()) {
    throw std::runtime_error("Torrent already exists");
  }

  torrent->tracker_thread =
      std::thread(&TorrentManager::tracker_worker, this, info_hash_hex);
  torrent->peer_thread =
      std::thread(&TorrentManager::peer_worker, this, info_hash_hex);

  torrents_[info_hash_hex] = std::move(torrent);

  // Save initial state with torrent data for restoration on restart
  // Only save if we didn't already load an existing state (to preserve
  // progress)
  if (!raw_torrent_data.empty() && !saved_state) {
    TorrentState state;
    state.info_hash_hex = info_hash_hex;
    state.torrent_data = raw_torrent_data;
    state.bitfield = torrents_[info_hash_hex]->piece_manager->get_bitfield();
    state.uploaded_bytes = 0;
    state.downloaded_bytes = 0;
    state.file_priorities = torrents_[info_hash_hex]->file_priorities;
    state.last_updated = std::chrono::system_clock::now();
    state_manager_.save_state(info_hash_hex, state);
  } else if (!raw_torrent_data.empty() && saved_state &&
             saved_state->torrent_data.empty()) {
    // If we loaded a state but it didn't have torrent_data, update it now
    TorrentState state = *saved_state;
    state.torrent_data = raw_torrent_data;
    state_manager_.save_state(info_hash_hex, state);
  }

  return info_hash_hex;
}

bool TorrentManager::remove_torrent(const std::string &torrent_id) {
  std::unique_ptr<Torrent> torrent;
  TorrentState state;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = torrents_.find(torrent_id);
    if (it == torrents_.end()) {
      return false;
    }

    // Prepare state - copy values that don't need piece_manager lock
    state.info_hash_hex = torrent_id;
    state.uploaded_bytes = it->second->uploaded_bytes;
    state.downloaded_bytes = it->second->downloaded_bytes;
    state.file_priorities = it->second->file_priorities;
    state.last_updated = std::chrono::system_clock::now();

    // Signal stop
    {
      std::lock_guard<std::mutex> t_lock(it->second->mutex);
      it->second->stopping = true;
      it->second->cv.notify_all();
    }

    // Move ownership out of map (piece_manager pointer remains valid)
    torrent = std::move(it->second);
    torrents_.erase(it);
  }
  // mutex_ released here

  // Get bitfield AFTER releasing mutex_ to avoid deadlock
  // (piece_manager is still valid since we own the torrent)
  state.bitfield = torrent->piece_manager->get_bitfield();
  state_manager_.save_state(torrent_id, state);

  // Stop threads outside the lock
  if (torrent->tracker_thread.joinable()) {
    torrent->tracker_thread.join();
  }
  if (torrent->peer_thread.joinable()) {
    torrent->peer_thread.join();
  }

  return true;
}

bool TorrentManager::pause_torrent(const std::string &torrent_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = torrents_.find(torrent_id);
  if (it == torrents_.end()) {
    return false;
  }

  it->second->paused = true;
  return true;
}

bool TorrentManager::resume_torrent(const std::string &torrent_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = torrents_.find(torrent_id);
  if (it == torrents_.end()) {
    return false;
  }

  it->second->paused = false;
  return true;
}

} // namespace pixelscrape
