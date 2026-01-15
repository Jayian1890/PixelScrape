#include "torrent_manager.hpp"
#include <json.hpp>
#include <random>
#include <chrono>

namespace pixelscrape {

TorrentManager::TorrentManager(const std::filesystem::path& download_dir, const std::filesystem::path& state_dir)
    : download_dir_(download_dir), state_manager_(state_dir) {}

TorrentManager::~TorrentManager() {
    running_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : torrents_) {
        if (pair.second->tracker_thread.joinable()) {
            pair.second->tracker_thread.join();
        }
        if (pair.second->peer_thread.joinable()) {
            pair.second->peer_thread.join();
        }
    }
    torrents_.clear();
}

std::string TorrentManager::add_torrent(const std::filesystem::path& torrent_path,
                                       const std::vector<size_t>& file_priorities) {
    auto metadata = TorrentMetadataParser::parse(torrent_path);
    return add_torrent_impl(std::move(metadata), file_priorities);
}

std::string TorrentManager::add_torrent_data(const std::string& data,
                                            const std::vector<size_t>& file_priorities) {
    auto metadata = TorrentMetadataParser::parse(data);
    return add_torrent_impl(std::move(metadata), file_priorities);
}

std::string TorrentManager::add_torrent_impl(TorrentMetadata metadata,
                                            const std::vector<size_t>& file_priorities) {
    std::string info_hash_hex = StateManager::info_hash_to_hex(metadata.info_hash);

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
    torrent->piece_manager = std::make_unique<PieceManager>(torrent->metadata, download_dir_);

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

    torrent->tracker_thread = std::thread(&TorrentManager::tracker_worker, this, info_hash_hex);
    torrent->peer_thread = std::thread(&TorrentManager::peer_worker, this, info_hash_hex);

    torrents_[info_hash_hex] = std::move(torrent);
    return info_hash_hex;
}

bool TorrentManager::remove_torrent(const std::string& torrent_id) {
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

    // Stop threads
    if (it->second->tracker_thread.joinable()) {
        it->second->tracker_thread.join();
    }
    if (it->second->peer_thread.joinable()) {
        it->second->peer_thread.join();
    }

    torrents_.erase(it);
    return true;
}

bool TorrentManager::pause_torrent(const std::string& torrent_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = torrents_.find(torrent_id);
    if (it == torrents_.end()) {
        return false;
    }

    it->second->paused = true;
    return true;
}

bool TorrentManager::resume_torrent(const std::string& torrent_id) {
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
    for (const auto& pair : torrents_) {
        ids.push_back(pair.first);
    }
    return ids;
}

std::optional<pixellib::core::json::JSON> TorrentManager::get_torrent_status(const std::string& torrent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = torrents_.find(torrent_id);
    if (it == torrents_.end()) {
        return std::nullopt;
    }

    const auto& torrent = it->second;
    pixellib::core::json::JSON status = pixellib::core::json::JSON::object({});

    status["name"] = pixellib::core::json::JSON(torrent->metadata.name);
    status["info_hash"] = pixellib::core::json::JSON(torrent_id);
    status["total_size"] = pixellib::core::json::JSON(static_cast<double>(torrent->metadata.total_length));
    status["downloaded"] = pixellib::core::json::JSON(static_cast<double>(torrent->piece_manager->get_total_downloaded_bytes()));
    status["uploaded"] = pixellib::core::json::JSON(static_cast<double>(torrent->uploaded_bytes));
    status["download_speed"] = pixellib::core::json::JSON(static_cast<double>(torrent->download_speed));
    status["upload_speed"] = pixellib::core::json::JSON(static_cast<double>(torrent->upload_speed));
    status["completion"] = pixellib::core::json::JSON(torrent->piece_manager->get_completion_percentage());
    status["paused"] = pixellib::core::json::JSON(torrent->paused);
    status["peers"] = pixellib::core::json::JSON(static_cast<double>(torrent->peers.size()));
    status["total_peers"] = pixellib::core::json::JSON(static_cast<double>(torrent->peers.size() + torrent->discovered_peers.size()));

    // Peer list
    pixellib::core::json::JSON peers_list = pixellib::core::json::JSON::array({});
    for (const auto& pc : torrent->peers) {
        if (pc->is_connected()) {
            pixellib::core::json::JSON peer_obj = pixellib::core::json::JSON::object({});
            const auto& info = pc->get_peer_info();
            peer_obj["ip"] = pixellib::core::json::JSON(info.to_string());
            peer_obj["port"] = pixellib::core::json::JSON(static_cast<double>(info.port));
            peer_obj["client"] = pixellib::core::json::JSON("BitTorrent Client");
            peer_obj["choking"] = pixellib::core::json::JSON(pc->is_choking());
            peers_list.push_back(peer_obj);
        }
    }
    status["peers_list"] = peers_list;

    // File list
    pixellib::core::json::JSON files = pixellib::core::json::JSON::array({});
    for (size_t i = 0; i < torrent->metadata.files.size(); ++i) {
        const auto& file = torrent->metadata.files[i];
        pixellib::core::json::JSON file_obj = pixellib::core::json::JSON::object({});
        file_obj["path"] = pixellib::core::json::JSON(file.path);
        file_obj["size"] = pixellib::core::json::JSON(static_cast<double>(file.length));
        file_obj["priority"] = pixellib::core::json::JSON(static_cast<double>(torrent->file_priorities[i]));
        files.push_back(file_obj);
    }
    status["files"] = files;

    return status;
}

void TorrentManager::update_speeds() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    for (auto& pair : torrents_) {
        auto& torrent = pair.second;
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - torrent->last_speed_update);
        
        if (duration.count() >= 500) { // Update every 0.5s or more
            size_t downloaded = torrent->piece_manager->get_total_downloaded_bytes();
            size_t uploaded = torrent->uploaded_bytes;

            size_t diff_down = (downloaded >= torrent->last_downloaded_bytes) ? (downloaded - torrent->last_downloaded_bytes) : 0;
            size_t diff_up = (uploaded >= torrent->last_uploaded_bytes) ? (uploaded - torrent->last_uploaded_bytes) : 0;

            torrent->download_speed = static_cast<size_t>(diff_down * 1000.0 / duration.count());
            torrent->upload_speed = static_cast<size_t>(diff_up * 1000.0 / duration.count());

            torrent->last_downloaded_bytes = downloaded;
            torrent->last_uploaded_bytes = uploaded;
            torrent->last_speed_update = now;
        }
    }
}

pixellib::core::json::JSON TorrentManager::get_global_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    pixellib::core::json::JSON stats = pixellib::core::json::JSON::object({});
    stats["total_torrents"] = pixellib::core::json::JSON(static_cast<double>(torrents_.size()));

    size_t total_downloaded = 0;
    size_t total_uploaded = 0;
    size_t active_peers = 0;
    size_t total_down_speed = 0;
    size_t total_up_speed = 0;

    for (const auto& pair : torrents_) {
        total_downloaded += pair.second->piece_manager->get_total_downloaded_bytes();
        total_uploaded += pair.second->uploaded_bytes;
        active_peers += pair.second->peers.size();
        total_down_speed += pair.second->download_speed;
        total_up_speed += pair.second->upload_speed;
    }

    stats["total_downloaded"] = pixellib::core::json::JSON(static_cast<double>(total_downloaded));
    stats["total_uploaded"] = pixellib::core::json::JSON(static_cast<double>(total_uploaded));
    stats["active_peers"] = pixellib::core::json::JSON(static_cast<double>(active_peers));
    stats["download_speed"] = pixellib::core::json::JSON(static_cast<double>(total_down_speed));
    stats["upload_speed"] = pixellib::core::json::JSON(static_cast<double>(total_up_speed));

    return stats;
}

void TorrentManager::tracker_worker(const std::string& torrent_id) {
    while (running_) { // Use a better check if possible, for now just while(true) or similar
        std::array<uint8_t, 20> peer_id;
        size_t uploaded, downloaded, left;
        TrackerClient* tracker = nullptr;
        bool paused = false;

        std::vector<std::string> trackers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = torrents_.find(torrent_id);
            if (it == torrents_.end()) break;

            peer_id = it->second->peer_id;
            uploaded = it->second->uploaded_bytes;
            downloaded = it->second->downloaded_bytes;
            left = it->second->metadata.total_length - downloaded;
            tracker = it->second->tracker.get();
            paused = it->second->paused;
            trackers = it->second->metadata.announce_list;
        }

        if (paused) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        for (const auto& announce_url : trackers) {
            try {
                auto peers = tracker->get_peers(peer_id, 6881, uploaded, downloaded, left, "", announce_url);
                
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = torrents_.find(torrent_id);
                if (it != torrents_.end()) {
                    auto& torrent = it->second;
                    for (const auto& peer_info : peers) {
                        bool known = false;
                        for (const auto& existing : torrent->discovered_peers) {
                            if (existing.ip == peer_info.ip && existing.port == peer_info.port) {
                                known = true;
                                break;
                            }
                        }
                        if (!known) {
                            torrent->discovered_peers.push_back(peer_info);
                        }
                    }
                }
            } catch (const std::exception& e) {
                pixellib::core::logging::Logger::error("Tracker error for {} ({}): {}", torrent_id, announce_url, e.what());
            }
        }

        std::this_thread::sleep_for(std::chrono::minutes(1));
    }
}

void TorrentManager::peer_worker(const std::string& torrent_id) {
    while (running_) {
        std::vector<PeerInfo> to_connect;
        std::array<uint8_t, 20> peer_id;
        std::array<uint8_t, 20> info_hash;
        TorrentMetadata* metadata = nullptr;
        PieceManager* piece_manager = nullptr;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = torrents_.find(torrent_id);
            if (it == torrents_.end()) break;

            auto& torrent = it->second;
            if (torrent->paused) {
                torrent->peers.clear();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            // 1. Get one peer to connect to at a time to avoid blocking piece management
            if (!torrent->discovered_peers.empty() && torrent->peers.size() < 50) {
                to_connect.push_back(torrent->discovered_peers.back());
                torrent->discovered_peers.pop_back();
            }
            
            peer_id = torrent->peer_id;
            info_hash = torrent->metadata.info_hash;
            metadata = &torrent->metadata;
            piece_manager = torrent->piece_manager.get();
        }

        // 2. Connect to new peers outside the lock
        for (const auto& peer_info : to_connect) {
            auto pc = std::make_unique<PeerConnection>(*metadata, info_hash, peer_id, peer_info, *piece_manager);
            if (pc->connect()) {
                if (pc->perform_handshake()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = torrents_.find(torrent_id);
                    if (it != torrents_.end()) {
                        it->second->peers.push_back(std::move(pc));
                        pixellib::core::logging::Logger::info("Connected to peer: {}", peer_info.to_string());
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = torrents_.find(torrent_id);
            if (it == torrents_.end()) break;
            auto& torrent = it->second;

            // 2. Cleanup disconnected peers
            for (auto it_pc = torrent->peers.begin(); it_pc != torrent->peers.end(); ) {
                if (!(*it_pc)->is_connected()) {
                    it_pc = torrent->peers.erase(it_pc);
                } else {
                    ++it_pc;
                }
            }

            // 3. Basic download coordination
            if (torrent->peers.empty()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            // Collect bitfields from connected and unchoked peers
            std::vector<std::vector<bool>> bitfields;
            for (const auto& pc : torrent->peers) {
                if (pc->is_connected() && !pc->is_choking()) {
                    bitfields.push_back(pc->get_bitfield());
                }
            }

            if (bitfields.empty()) {
                // If everyone is choking us, we still need to tell them we are interested
                for (auto& pc : torrent->peers) {
                    if (pc->is_connected() && !pc->is_interested()) {
                        pc->set_interested(true);
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            size_t piece_index = torrent->piece_manager->select_rarest_piece(bitfields);
            if (piece_index != SIZE_MAX) {
                // Find a peer that has this piece
                for (auto& pc : torrent->peers) {
                    if (pc->is_connected() && !pc->is_choking() && pc->get_bitfield()[piece_index]) {
                        // Request blocks for this piece
                        size_t block_size = 16384;
                        size_t piece_size = (piece_index == torrent->metadata.piece_hashes.size() - 1) ?
                            (torrent->metadata.total_length % torrent->metadata.piece_length) : torrent->metadata.piece_length;
                        if (piece_size == 0) piece_size = torrent->metadata.piece_length;
                        
                        size_t num_blocks = (piece_size + block_size - 1) / block_size;
                        
                        bool any_requested = false;
                        for (size_t b = 0; b < num_blocks; ++b) {
                            if (torrent->piece_manager->request_block(piece_index, b, block_size)) {
                                size_t offset = b * block_size;
                                size_t len = std::min(block_size, piece_size - offset);
                                pc->send_request(piece_index, offset, len);
                                any_requested = true;
                            }
                        }
                        
                        if (any_requested) {
                            pixellib::core::logging::Logger::info("Requested piece {} from a peer", piece_index);
                            break; // Moved to next piece
                        }
                    }
                }
            }

            // 4. Verify completed pieces
            for (size_t i = 0; i < torrent->metadata.piece_hashes.size(); ++i) {
                if (!torrent->piece_manager->get_bitfield()[i] && torrent->piece_manager->is_piece_complete(i)) {
                    if (torrent->piece_manager->verify_piece(i)) {
                        pixellib::core::logging::Logger::info("Piece {} verified and completed", i);
                        torrent->downloaded_bytes += (i == torrent->metadata.piece_hashes.size() - 1) ?
                            (torrent->metadata.total_length % torrent->metadata.piece_length) : torrent->metadata.piece_length;
                        
                        // Broadcast HAVEs to all peers
                        for (auto& pc : torrent->peers) {
                            if (pc->is_connected()) pc->send_have(i);
                        }
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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