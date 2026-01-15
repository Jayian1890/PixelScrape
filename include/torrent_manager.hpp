#pragma once

#include "torrent_metadata.hpp"
#include "tracker_client.hpp"
#include "peer_connection.hpp"
#include "piece_manager.hpp"
#include "state_manager.hpp"
#include <logging.hpp>
#include <json.hpp>
#include <memory>
#include <unordered_map>
#include <thread>
#include <atomic>

namespace pixelscrape {

struct Torrent {
    TorrentMetadata metadata;
    std::unique_ptr<TrackerClient> tracker;
    std::unique_ptr<PieceManager> piece_manager;
    std::vector<std::shared_ptr<PeerConnection>> peers;
    std::vector<PeerInfo> discovered_peers;
    std::array<uint8_t, 20> peer_id;
    std::thread tracker_thread;
    std::thread peer_thread;
    std::atomic<size_t> uploaded_bytes{0};
    std::atomic<size_t> downloaded_bytes{0};
    std::vector<size_t> file_priorities;
    bool paused{false};

    std::mutex mutex;
    std::condition_variable cv;
    bool stopping{false};
};

class TorrentManager {
public:
    TorrentManager(const std::filesystem::path& download_dir, const std::filesystem::path& state_dir);
    ~TorrentManager();

    // Torrent management
    std::string add_torrent(const std::filesystem::path& torrent_path,
                           const std::vector<size_t>& file_priorities = {});
    std::string add_torrent_data(const std::string& data,
                                const std::vector<size_t>& file_priorities = {});
    bool remove_torrent(const std::string& torrent_id);
    bool pause_torrent(const std::string& torrent_id);
    bool resume_torrent(const std::string& torrent_id);

    // Status queries
    std::vector<std::string> list_torrents() const;
    std::optional<pixellib::core::json::JSON> get_torrent_status(const std::string& torrent_id) const;

    // Statistics
    pixellib::core::json::JSON get_global_stats() const;

private:
    void tracker_worker(const std::string& torrent_id);
    void peer_worker(const std::string& torrent_id);
    std::string add_torrent_impl(TorrentMetadata metadata,
                                const std::vector<size_t>& file_priorities);
    std::array<uint8_t, 20> generate_peer_id();

    std::unordered_map<std::string, std::unique_ptr<Torrent>> torrents_;
    std::filesystem::path download_dir_;
    StateManager state_manager_;
    mutable std::mutex mutex_;
};

} // namespace pixelscrape