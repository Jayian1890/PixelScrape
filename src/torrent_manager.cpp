#include "torrent_manager.hpp"
#include "filesystem.hpp"
#include <chrono>
#include <json.hpp>
#include <random>
#include <unordered_set>

namespace {
std::string peer_key(const pixelscrape::PeerInfo &peer) {
  std::string key;
  key.resize(6);
  key[0] = static_cast<char>(peer.ip[0]);
  key[1] = static_cast<char>(peer.ip[1]);
  key[2] = static_cast<char>(peer.ip[2]);
  key[3] = static_cast<char>(peer.ip[3]);
  key[4] = static_cast<char>((peer.port >> 8) & 0xFF);
  key[5] = static_cast<char>(peer.port & 0xFF);
  return key;
}

std::unordered_set<std::string>
build_peer_set(const std::vector<pixelscrape::PeerInfo> &peers) {
  std::unordered_set<std::string> seen;
  seen.reserve(peers.size());
  for (const auto &peer : peers) {
    seen.insert(peer_key(peer));
  }
  return seen;
}

size_t add_unique_peers(std::vector<pixelscrape::PeerInfo> &existing_peers,
                        const std::vector<pixelscrape::PeerInfo> &new_peers) {
  auto seen = build_peer_set(existing_peers);
  size_t added_count = 0;
  for (const auto &np : new_peers) {
    if (seen.insert(peer_key(np)).second) {
      existing_peers.push_back(np);
      added_count++;
    }
  }
  return added_count;
}
} // namespace

namespace pixelscrape {

TorrentManager::TorrentManager(const std::string &download_dir,
                               const std::string &state_dir)
    : download_dir_(download_dir), state_manager_(state_dir) {

  // Initialize DHT client
  dht_client_ = std::make_unique<dht::DHTClient>(6881);
  dht_client_->start(state_dir);

  // Start connection worker thread
  connection_thread_ = std::thread(&TorrentManager::connection_worker, this);

  // Start verification worker thread
  verification_thread_ = std::thread(&TorrentManager::verification_worker, this);
}

TorrentManager::~TorrentManager() {
  // Stop connection worker
  {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    connection_worker_running_ = false;
    connection_cv_.notify_one();
  }
  if (connection_thread_.joinable()) {
    connection_thread_.join();
  }

  // Stop verification worker
  {
    std::lock_guard<std::mutex> lock(verification_mutex_);
    verification_worker_running_ = false;
    verification_cv_.notify_one();
  }
  if (verification_thread_.joinable()) {
    verification_thread_.join();
  }

  if (dht_client_) {
    dht_client_->stop();
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

std::string
TorrentManager::add_torrent(const std::string &torrent_path,
                            const std::vector<size_t> &file_priorities) {
  std::string data =
      pixellib::core::filesystem::FileSystem::read_file(torrent_path);
  auto metadata = TorrentMetadataParser::parse(data);
  return add_torrent_impl(std::move(metadata), file_priorities);
}

std::string
TorrentManager::add_torrent_data(const std::string &data,
                                 const std::vector<size_t> &file_priorities) {
  auto metadata = TorrentMetadataParser::parse(data);
  return add_torrent_impl(std::move(metadata), file_priorities);
}

std::string TorrentManager::add_torrent_impl(TorrentMetadata metadata,
                                             const std::vector<size_t> &file_priorities) {
  std::string torrent_id = StateManager::info_hash_to_hex(metadata.info_hash);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (torrents_.find(torrent_id) != torrents_.end()) {
      throw std::runtime_error("Torrent already exists");
    }
  }

  // Create torrent object
  auto torrent = std::make_unique<Torrent>();
  torrent->metadata = std::move(metadata);
  torrent->peer_id = generate_peer_id();
  torrent->file_priorities = file_priorities.empty()
                                 ? std::vector<size_t>(torrent->metadata.files.size(), 1)
                                 : file_priorities;

  // Create piece manager
  torrent->piece_manager = std::make_unique<PieceManager>(
      torrent->metadata, download_dir_, torrent->file_priorities);

  // Create tracker client
  torrent->tracker = std::make_unique<TrackerClient>(torrent->metadata);

  // Load saved state if available
  if (auto saved_state = state_manager_.load_state(torrent_id)) {
    torrent->piece_manager->load_state(saved_state->bitfield);
    torrent->uploaded_bytes = saved_state->uploaded_bytes;
    torrent->downloaded_bytes = saved_state->downloaded_bytes;
    torrent->file_priorities = saved_state->file_priorities;
  }

  // Check if we have pending peers from magnet link
  std::vector<PeerInfo> pending_peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_magnet_peers_.find(torrent_id);
    if (it != pending_magnet_peers_.end()) {
      pending_peers = std::move(it->second);
      pending_magnet_peers_.erase(it);
    }
  }

  // Add pending peers to discovered peers
  if (!pending_peers.empty()) {
    std::lock_guard<std::mutex> t_lock(torrent->mutex);
    torrent->discovered_peers.insert(torrent->discovered_peers.end(),
                                     pending_peers.begin(), pending_peers.end());
  }

  // Start worker threads
  torrent->tracker_thread =
      std::thread(&TorrentManager::tracker_worker, this, torrent_id);
  torrent->peer_thread =
      std::thread(&TorrentManager::peer_worker, this, torrent_id);

  // Add to torrents map
  {
    std::lock_guard<std::mutex> lock(mutex_);
    torrents_[torrent_id] = std::move(torrent);
  }

  pixellib::core::logging::Logger::info("Added torrent: {}", torrent_id);
  return torrent_id;
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
          pixellib::core::logging::Logger::info(
              "DHT discovered {} peers for torrent {}", peers.size(),
              torrent_id);

          // Convert to PeerInfo
          std::vector<PeerInfo> new_peers;
          for (const auto &dht_peer : peers) {
            PeerInfo pi;
            pi.ip = dht_peer.ip;
            pi.port = dht_peer.port;
            new_peers.push_back(pi);
          }

          // First, determine whether the torrent exists while holding the
          // manager mutex, and update pending lists if it does not.
          Torrent *torrent = nullptr;
          {
            std::lock_guard<std::mutex> lock(mutex_);

            // If torrent already exists (metadata fetched), grab a shared
            // pointer so we can operate on it after releasing mutex_.
            auto it = torrents_.find(torrent_id);
            if (it != torrents_.end()) {
              torrent = it->second.get();
            } else {
              // Store in pending list until metadata is fetched
              auto &pending = pending_magnet_peers_[torrent_id];
              size_t added_count = add_unique_peers(pending, new_peers);
              if (added_count > 0) {
                pixellib::core::logging::Logger::info(
                    "DHT buffered {} new peers for pending magnet {}",
                    added_count, torrent_id);
              }
            }
          }

          // If we resolved an active torrent, update its discovered peers
          // under the torrent's own mutex, without holding mutex_.
          if (torrent) {
            std::lock_guard<std::mutex> t_lock(torrent->mutex);
            size_t added_count = add_unique_peers(torrent->discovered_peers, new_peers);
            if (added_count > 0) {
              pixellib::core::logging::Logger::info(
                  "DHT discovered {} new peers for active torrent {}",
                  added_count, torrent_id);
            }
          }
        });
  }

  return torrent_id;
}

bool TorrentManager::remove_torrent(const std::string &torrent_id) {
  std::unique_ptr<Torrent> torrent;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = torrents_.find(torrent_id);
    if (it == torrents_.end()) {
      return false;
    }

    // Save state before removing
    TorrentState state;
    state.info_hash_hex = torrent_id;
    state.bitfield = it->second->piece_manager->get_bitfield();
    state.uploaded_bytes = it->second->uploaded_bytes;
    state.downloaded_bytes = it->second->downloaded_bytes;
    state.file_priorities = it->second->file_priorities;
    state.last_updated = std::chrono::system_clock::now();

    state_manager_.save_state(torrent_id, state);

    // Signal stop
    {
      std::lock_guard<std::mutex> t_lock(it->second->mutex);
      it->second->stopping = true;
      it->second->cv.notify_all();
    }

    // Move ownership out of map
    torrent = std::move(it->second);
    torrents_.erase(it);
  }

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
        size_t added_count = add_unique_peers(torrent->discovered_peers, peers);
        total_new_peers += added_count;

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

void TorrentManager::connection_worker() {
  std::vector<ConnectionRequest> active_requests;

  while (connection_worker_running_) {
    // Add new connection requests
    {
      std::lock_guard<std::mutex> lock(connection_mutex_);
      while (!connection_queue_.empty()) {
        auto request = std::move(connection_queue_.front());
        connection_queue_.pop();

        // Initialize the connection state
        request.state = TorrentManager::ConnectionState::CONNECTING;
        request.start_time = std::chrono::steady_clock::now();

        // Start the connection
        if (request.peer_connection->connect()) {
          active_requests.push_back(std::move(request));
        } else {
          // Connection failed immediately
          pixellib::core::logging::Logger::debug(
              "Connection failed immediately to peer {}.{}.{}.{}:{} for torrent {}",
              static_cast<int>(request.peer_info.ip[0]),
              static_cast<int>(request.peer_info.ip[1]),
              static_cast<int>(request.peer_info.ip[2]),
              static_cast<int>(request.peer_info.ip[3]),
              request.peer_info.port, request.torrent_id.substr(0, 8));
        }
      }
    }

    if (active_requests.empty()) {
      // Wait for new requests
      std::unique_lock<std::mutex> lock(connection_mutex_);
      connection_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return !connection_queue_.empty() || !connection_worker_running_;
      });
      continue;
    }

    // Check for completed connections and timeouts
    auto now = std::chrono::steady_clock::now();
    std::vector<size_t> completed_indices;

    for (size_t i = 0; i < active_requests.size(); ++i) {
      auto &request = active_requests[i];

      // Check for timeout (10 seconds)
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          now - request.start_time);
      if (elapsed.count() >= 10) {
        pixellib::core::logging::Logger::debug(
            "Connection timeout to peer {}.{}.{}.{}:{} for torrent {}",
            static_cast<int>(request.peer_info.ip[0]),
            static_cast<int>(request.peer_info.ip[1]),
            static_cast<int>(request.peer_info.ip[2]),
            static_cast<int>(request.peer_info.ip[3]),
            request.peer_info.port, request.torrent_id.substr(0, 8));
        request.state = TorrentManager::ConnectionState::FAILED;
        completed_indices.push_back(i);
        continue;
      }

      if (request.state == TorrentManager::ConnectionState::CONNECTING) {
        if (request.peer_connection->is_connect_complete()) {
          // Connection established, start handshake
          if (request.peer_connection->start_handshake()) {
            request.state = TorrentManager::ConnectionState::HANDSHAKING;
          } else {
            request.state = TorrentManager::ConnectionState::FAILED;
            completed_indices.push_back(i);
          }
        }
      } else if (request.state == TorrentManager::ConnectionState::HANDSHAKING) {
        if (request.peer_connection->continue_handshake()) {
          if (request.peer_connection->is_handshake_complete()) {
            if (request.peer_connection->handshake_successful()) {
              request.state = TorrentManager::ConnectionState::COMPLETED;
            } else {
              request.state = TorrentManager::ConnectionState::FAILED;
            }
            completed_indices.push_back(i);
          }
        }
      }
    }

    // Process completed requests
    for (auto it = completed_indices.rbegin(); it != completed_indices.rend(); ++it) {
      size_t index = *it;
      auto request = std::move(active_requests[index]);

      if (request.state == TorrentManager::ConnectionState::COMPLETED) {
        // Connection successful, add to torrent's peer list
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = torrents_.find(request.torrent_id);
        if (it != torrents_.end()) {
          std::lock_guard<std::mutex> t_lock(it->second->mutex);
          it->second->peers.push_back(request.peer_connection);

          // Start the peer's message handling thread now that handshake is complete
          // Set socket back to blocking for the message thread
          // Note: We need to modify PeerConnection to handle this properly
          request.peer_connection->start_message_thread();

          pixellib::core::logging::Logger::info(
              "Successfully connected to peer {}.{}.{}.{}:{} for torrent {}",
              static_cast<int>(request.peer_info.ip[0]),
              static_cast<int>(request.peer_info.ip[1]),
              static_cast<int>(request.peer_info.ip[2]),
              static_cast<int>(request.peer_info.ip[3]),
              request.peer_info.port, request.torrent_id.substr(0, 8));
        }
      } else {
        pixellib::core::logging::Logger::debug(
            "Connection/handshake failed to peer {}.{}.{}.{}:{} for torrent {}",
            static_cast<int>(request.peer_info.ip[0]),
            static_cast<int>(request.peer_info.ip[1]),
            static_cast<int>(request.peer_info.ip[2]),
            static_cast<int>(request.peer_info.ip[3]),
            request.peer_info.port, request.torrent_id.substr(0, 8));
      }

      active_requests.erase(active_requests.begin() + index);
    }

    // Small delay to prevent busy loop
    if (!active_requests.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  // Clean up any remaining active requests
  active_requests.clear();
}

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

      // Submit connection requests to the connection worker
      for (const auto &peer_info : discovered_copy) {
        if (peers_copy.size() >= 50)
          break;

        // Create new connection with encryption and extensions enabled
        auto peer_conn = std::make_shared<PeerConnection>(
            torrent->metadata, torrent->metadata.info_hash, torrent->peer_id,
            peer_info, *torrent->piece_manager, true,
            true); // enable_encryption, enable_extensions

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
                // Submit verification request to the verification worker
                {
                  std::lock_guard<std::mutex> v_lock(verification_mutex_);
                  verification_queue_.push({torrent_id, index});
                  verification_cv_.notify_one();
                }
              }
            }
          }
        });

        // Submit connection request to the connection worker
        {
          std::lock_guard<std::mutex> lock(connection_mutex_);
          connection_queue_.push({torrent_id, peer_info, peer_conn});
          connection_cv_.notify_one();
        }
      }

      // Cleanup disconnected peers
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

void TorrentManager::verification_worker() {
  while (verification_worker_running_) {
    VerificationRequest request;

    {
      std::unique_lock<std::mutex> lock(verification_mutex_);
      verification_cv_.wait(lock, [this]() {
        return !verification_queue_.empty() || !verification_worker_running_;
      });

      if (!verification_worker_running_) {
        break;
      }

      if (verification_queue_.empty()) {
        continue;
      }

      request = std::move(verification_queue_.front());
      verification_queue_.pop();
    }

    // Process the verification request
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = torrents_.find(request.torrent_id);
    if (it != torrents_.end()) {
      if (it->second->piece_manager->verify_piece(request.piece_index)) {
        // Piece verified successfully!
        // Send HAVE to all peers
        auto peers_snapshot = it->second->peers;
        for (auto &peer : peers_snapshot) {
          if (peer->is_connected()) {
            peer->set_have_piece(request.piece_index, true);
          }
        }

        pixellib::core::logging::Logger::info(
            "Piece {} verified successfully for torrent {}",
            request.piece_index, request.torrent_id.substr(0, 8));
      } else {
        pixellib::core::logging::Logger::warning(
            "Piece {} verification failed for torrent {}",
            request.piece_index, request.torrent_id.substr(0, 8));
      }
    }
  }
}

std::array<uint8_t, 20> TorrentManager::generate_peer_id() {
  std::array<uint8_t, 20> peer_id;
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);

  // BitTorrent peer ID format: -PS0001-XXXXXXXXXXXX
  peer_id[0] = '-';
  peer_id[1] = 'P';
  peer_id[2] = 'S';
  peer_id[3] = '0';
  peer_id[4] = '0';
  peer_id[5] = '0';
  peer_id[6] = '1';
  peer_id[7] = '-';

  for (size_t i = 8; i < 20; ++i) {
    peer_id[i] = dis(gen);
  }

  return peer_id;
}

} // namespace pixelscrape