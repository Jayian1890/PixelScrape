#include "torrent_manager.hpp"
#include <json.hpp>
#include <random>
#include <chrono>

namespace pixelscrape {

TorrentManager::TorrentManager(const std::filesystem::path& download_dir, const std::filesystem::path& state_dir)
    : download_dir_(download_dir), state_manager_(state_dir) {}

TorrentManager::~TorrentManager() {
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

    std::lock_guard<std::mutex> lock(mutex_);

    if (torrents_.find(info_hash_hex) != torrents_.end()) {
        throw std::runtime_error("Torrent already exists");
    }

    auto torrent = std::make_unique<Torrent>();
    torrent->metadata = std::move(metadata);
    torrent->peer_id = generate_peer_id();
    torrent->tracker = std::make_unique<TrackerClient>(torrent->metadata);
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
    status["downloaded"] = pixellib::core::json::JSON(static_cast<double>(torrent->downloaded_bytes));
    status["uploaded"] = pixellib::core::json::JSON(static_cast<double>(torrent->uploaded_bytes));
    status["completion"] = pixellib::core::json::JSON(torrent->piece_manager->get_completion_percentage());
    status["paused"] = pixellib::core::json::JSON(torrent->paused);
    status["peers"] = pixellib::core::json::JSON(static_cast<double>(torrent->peers.size()));

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

pixellib::core::json::JSON TorrentManager::get_global_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    pixellib::core::json::JSON stats = pixellib::core::json::JSON::object({});
    stats["total_torrents"] = pixellib::core::json::JSON(static_cast<double>(torrents_.size()));

    size_t total_downloaded = 0;
    size_t total_uploaded = 0;
    size_t active_peers = 0;

    for (const auto& pair : torrents_) {
        total_downloaded += pair.second->downloaded_bytes;
        total_uploaded += pair.second->uploaded_bytes;
        active_peers += pair.second->peers.size();
    }

    stats["total_downloaded"] = pixellib::core::json::JSON(static_cast<double>(total_downloaded));
    stats["total_uploaded"] = pixellib::core::json::JSON(static_cast<double>(total_uploaded));
    stats["active_peers"] = pixellib::core::json::JSON(static_cast<double>(active_peers));

    return stats;
}

void TorrentManager::tracker_worker(const std::string& torrent_id) {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = torrents_.find(torrent_id);
            if (it == torrents_.end()) {
                break; // Torrent removed
            }

            auto& torrent = it->second;
            if (torrent->paused) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            try {
                auto peers = torrent->tracker->get_peers(
                    torrent->peer_id,
                    6881, // Default port
                    torrent->uploaded_bytes,
                    torrent->downloaded_bytes,
                    torrent->metadata.total_length - torrent->downloaded_bytes,
                    "" // No event for now
                );

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

            } catch (const std::exception& e) {
                pixellib::core::logging::Logger::error("Tracker error for {}: {}", torrent_id, e.what());
            }
        }

        std::this_thread::sleep_for(std::chrono::minutes(5)); // Tracker update interval
    }
}

void TorrentManager::peer_worker(const std::string& torrent_id) {
    //TODO: Peer management would go here
    // This is a placeholder for the full implementation
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