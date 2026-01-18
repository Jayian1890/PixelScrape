#pragma once

#include "dht_node.hpp"
#include "dht_protocol.hpp"
#include <atomic>
#include <memory>

#include <condition_variable>
#include <functional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace pixelscrape {
namespace dht {

// Configuration constants
struct DHTConfig {
  static constexpr uint16_t DEFAULT_PORT = 6881;
  static constexpr size_t MAX_NODES = 2000;
  static constexpr size_t BOOTSTRAP_NODES_COUNT = 8;
  static constexpr size_t LOOKUP_K = 8;
  static constexpr size_t LOOKUP_ALPHA = 3;
  static constexpr int QUERY_TIMEOUT_SECONDS = 10;
  static constexpr int BOOTSTRAP_RETRY_SECONDS = 30;
  static constexpr int ROUTING_TABLE_REFRESH_MINUTES = 15;
  static constexpr int TOKEN_ROTATION_MINUTES = 5;
  static constexpr size_t MAX_QUERIES_PER_SECOND = 100;

  // Bootstrap nodes (public DHT routers)
  static const std::vector<std::pair<std::string, uint16_t>> &bootstrap_nodes();
};

// Peer info for torrents
struct TorrentPeer {
  std::array<uint8_t, 4> ip;
  uint16_t port;
};

struct StoredPeer {
  std::array<uint8_t, 4> ip;
  uint16_t port;
  std::chrono::steady_clock::time_point last_announced;
};

// Callbacks
using PeerDiscoveryCallback =
    std::function<void(const std::array<uint8_t, 20> &info_hash,
                       const std::vector<TorrentPeer> &peers)>;

// Unified iterative lookup state
struct LookupState {
  TransactionID id; // internal ID for this lookup
  NodeID target;
  QueryType type;                                      // FIND_NODE or GET_PEERS
  std::map<std::string, std::string> collected_tokens; // Node key -> Token
  std::optional<std::array<uint8_t, 20>> info_hash;    // For GET_PEERS/ANNOUNCE

  // Search state
  std::vector<DHTNode> shortlist;
  std::unordered_set<std::string> queried_nodes;
  std::unordered_set<std::string> responding_nodes;
  size_t in_flight{0};

  // Completion callbacks
  std::function<void(const std::vector<DHTNode> &)>
      nodes_callback;                   // For FIND_NODE
  PeerDiscoveryCallback peers_callback; // For GET_PEERS
  std::function<void(const std::vector<DHTNode> &)>
      announce_callback; // For ANNOUNCE phase 1

  std::chrono::steady_clock::time_point started_at;
};

// Pending query tracker
struct PendingQuery {
  TransactionID transaction_id;
  QueryType query_type;
  std::chrono::steady_clock::time_point sent_time;
  std::array<uint8_t, 4> target_ip;
  uint16_t target_port;

  // For iterative lookups
  std::shared_ptr<LookupState> associated_lookup;
  std::function<void(const ResponseMessage &)> callback;
};

// DHT Client - main interface
class DHTClient {
public:
  explicit DHTClient(uint16_t port = DHTConfig::DEFAULT_PORT);
  ~DHTClient();

  // Start/stop DHT
  bool start(const std::string &state_dir);
  void stop();
  bool is_running() const { return running_; }

  // Peer discovery
  void find_peers(const std::array<uint8_t, 20> &info_hash,
                  PeerDiscoveryCallback callback);

  // Announce that we're downloading/seeding a torrent
  void announce(const std::array<uint8_t, 20> &info_hash, uint16_t port);

  // Statistics
  size_t node_count() const;
  size_t active_queries() const;
  bool is_bootstrapped() const { return bootstrapped_; }

  const NodeID &own_id() const { return own_id_; }

private:
  // Core DHT operations
  void run();
  void process_incoming_messages();
  void handle_query(const QueryMessage &query,
                    const std::array<uint8_t, 4> &from_ip, uint16_t from_port);
  void handle_response(const ResponseMessage &response);
  void handle_error(const ErrorMessage &error);

  // Bootstrap
  void bootstrap();
  bool load_routing_table(const std::string &state_dir);
  void save_routing_table();

  // Iterative lookups
  void iterative_find_node(const NodeID &target);
  void iterative_get_peers(const std::array<uint8_t, 20> &info_hash,
                           PeerDiscoveryCallback callback);

  // Query sending
  void send_ping(const DHTNode &node);
  void send_find_node(const DHTNode &node, const NodeID &target,
                      std::shared_ptr<LookupState> lookup = nullptr);
  void
  send_get_peers(const DHTNode &node, const std::array<uint8_t, 20> &info_hash,
                 std::function<void(const ResponseMessage &)> callback = {},
                 std::shared_ptr<LookupState> lookup = nullptr);
  void send_announce_peer(const DHTNode &node,
                          const std::array<uint8_t, 20> &info_hash,
                          uint16_t port, const std::string &token);

  // Query tracking
  void track_query(const TransactionID &tid, const PendingQuery &query);
  std::optional<PendingQuery> get_pending_query(const TransactionID &tid);
  void remove_pending_query(const TransactionID &tid);
  void cleanup_expired_queries();

  // Iterative lookup helpers
  void start_lookup(std::shared_ptr<LookupState> lookup);
  void continue_lookup(std::shared_ptr<LookupState> lookup);
  void handle_lookup_response(const TransactionID &tid,
                              const ResponseMessage &response);

  // Maintenance
  void maintenance_loop();
  void refresh_routing_table();
  void check_node_health();

  // Rate limiting
  bool check_rate_limit(const std::array<uint8_t, 4> &ip);

  // Peer Storage
  void store_peer(const std::array<uint8_t, 20> &info_hash,
                  const std::array<uint8_t, 4> &ip, uint16_t port);
  void expire_peers();

  // Network
  void send_message(const std::vector<uint8_t> &data,
                    const std::array<uint8_t, 4> &ip, uint16_t port);

  // State
  NodeID own_id_;
  uint16_t port_;
  int socket_fd_;
  std::string state_dir_;

  // Routing
  std::unique_ptr<RoutingTable> routing_table_;
  TokenManager token_manager_;
  TransactionIDGenerator tid_generator_;

  // Queries
  std::unordered_map<TransactionID, PendingQuery> pending_queries_;
  std::mutex queries_mutex_;

  // Peer discovery callbacks
  std::unordered_map<std::string, PeerDiscoveryCallback> peer_callbacks_;
  std::mutex callbacks_mutex_;

  // Peer Storage
  using PeerList = std::vector<StoredPeer>;
  std::unordered_map<std::string, PeerList> stored_peers_;
  std::mutex storage_mutex_;

  // Unified iterative lookup state

  std::unordered_map<TransactionID, std::shared_ptr<LookupState>>
      active_lookups_;
  std::mutex lookups_mutex_;

  // Rate limiting (IP -> query count in current second)
  std::unordered_map<std::string,
                     std::pair<size_t, std::chrono::steady_clock::time_point>>
      rate_limits_;
  std::mutex rate_limit_mutex_;

  // Threading
  std::atomic<bool> running_;
  std::atomic<bool> bootstrapped_;
  std::thread network_thread_;
  std::thread maintenance_thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

} // namespace dht
} // namespace pixelscrape
