#pragma once

#include "dht_client.hpp"
#include "peer_connection.hpp"
#include "piece_manager.hpp"
#include "state_manager.hpp"
#include "torrent_metadata.hpp"
#include "tracker_client.hpp"
#include <atomic>
#include <chrono>
#include <json.hpp>
#include <logging.hpp>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace pixelscrape {

struct Torrent {
  TorrentMetadata metadata;
  std::unique_ptr<TrackerClient> tracker;
  std::unique_ptr<PieceManager> piece_manager;
  std::vector<std::shared_ptr<PeerConnection>> peers;
  std::vector<PeerInfo> discovered_peers;
  std::unordered_set<std::string> pending_connections; // Key: peer_key()
  std::array<uint8_t, 20> peer_id;
  std::thread tracker_thread;
  std::thread peer_thread;
  std::atomic<size_t> uploaded_bytes{0};
  std::atomic<size_t> downloaded_bytes{0};
  std::atomic<size_t> download_speed{0};
  std::atomic<size_t> upload_speed{0};
  size_t last_downloaded_bytes{0};
  size_t last_uploaded_bytes{0};
  std::chrono::steady_clock::time_point last_speed_update{
      std::chrono::steady_clock::now()};
  std::vector<size_t> file_priorities;
  bool paused{false};

  std::mutex mutex;
  std::condition_variable cv;
  bool stopping{false};
};

class TorrentManager {
public:
  TorrentManager(const std::string &download_dir, const std::string &state_dir);
  ~TorrentManager();

  // Torrent management
  std::string add_torrent(const std::string &torrent_path,
                          const std::vector<size_t> &file_priorities = {});
  std::string add_torrent_data(const std::string &data,
                               const std::vector<size_t> &file_priorities = {});
  std::string add_magnet_link(const std::string &magnet_uri);
  bool remove_torrent(const std::string &torrent_id);
  bool pause_torrent(const std::string &torrent_id);
  bool resume_torrent(const std::string &torrent_id);

  // Status queries
  std::vector<std::string> list_torrents() const;
  std::optional<pixellib::core::json::JSON>
  get_torrent_status(const std::string &torrent_id) const;

  // Statistics
  void update_speeds();
  pixellib::core::json::JSON get_global_stats() const;

private:
  void tracker_worker(const std::string &torrent_id);
  void peer_worker(const std::string &torrent_id);
  void connection_worker();
  void verification_worker();
  std::string add_torrent_impl(TorrentMetadata metadata,
                               const std::vector<size_t> &file_priorities);
  std::array<uint8_t, 20> generate_peer_id();

  std::unordered_map<std::string, std::unique_ptr<Torrent>> torrents_;
  std::unordered_map<std::string, std::vector<PeerInfo>> pending_magnet_peers_;
  std::string download_dir_;
  StateManager state_manager_;
  std::unique_ptr<dht::DHTClient> dht_client_;
  std::atomic<bool> running_{true};
  mutable std::mutex mutex_;

  // Connection management
  enum class ConnectionState { CONNECTING, HANDSHAKING, COMPLETED, FAILED };

  struct ConnectionRequest {
    std::string torrent_id;
    PeerInfo peer_info;
    std::shared_ptr<PeerConnection> peer_connection;
    ConnectionState state;
    std::chrono::steady_clock::time_point start_time;
  };
  std::queue<ConnectionRequest> connection_queue_;
  std::mutex connection_mutex_;
  std::condition_variable connection_cv_;
  std::thread connection_thread_;
  std::atomic<bool> connection_worker_running_{true};

  // TCP listener for incoming peer connections
  void tcp_listener(uint16_t port = 6881);
  std::thread tcp_listener_thread_;
  std::atomic<bool> tcp_listener_running_{false};

  // Verification management
  struct VerificationRequest {
    std::string torrent_id;
    size_t piece_index;
  };
  std::queue<VerificationRequest> verification_queue_;
  std::mutex verification_mutex_;
  std::condition_variable verification_cv_;
  std::thread verification_thread_;
  std::atomic<bool> verification_worker_running_{true};
};

} // namespace pixelscrape