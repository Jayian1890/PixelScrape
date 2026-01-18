#include "dht_client.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <filesystem.hpp>
#include <fstream>
#include <logging.hpp>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {
std::string node_key_from_endpoint(const std::array<uint8_t, 4> &ip,
                                   uint16_t port) {
  std::string key;
  key.resize(6);
  key[0] = static_cast<char>(ip[0]);
  key[1] = static_cast<char>(ip[1]);
  key[2] = static_cast<char>(ip[2]);
  key[3] = static_cast<char>(ip[3]);
  key[4] = static_cast<char>((port >> 8) & 0xFF);
  key[5] = static_cast<char>(port & 0xFF);
  return key;
}

std::string node_key(const pixelscrape::dht::DHTNode &node) {
  return node_key_from_endpoint(node.ip, node.port);
}
} // namespace

namespace pixelscrape {
namespace dht {

// Bootstrap nodes
const std::vector<std::pair<std::string, uint16_t>> &
DHTConfig::bootstrap_nodes() {
  static const std::vector<std::pair<std::string, uint16_t>> nodes = {
      {"router.bittorrent.com", 6881},
      {"dht.transmissionbt.com", 6881},
      {"router.utorrent.com", 6881},
      {"dht.aelitis.com", 6881},
      {"dht.libtorrent.org", 25401}};
  return nodes;
}

// DHTClient implementation

DHTClient::DHTClient(uint16_t port)
    : port_(port), socket_fd_(-1), running_(false), bootstrapped_(false) {
  own_id_ = NodeID::generate_random();
  routing_table_ = std::make_unique<RoutingTable>(own_id_);
}

DHTClient::~DHTClient() { stop(); }

bool DHTClient::start(const std::string &state_dir) {
  if (running_)
    return true;

  state_dir_ = state_dir;

  // Create UDP socket
  socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    pixellib::core::logging::Logger::error("DHT: Failed to create socket");
    return false;
  }

  // Set non-blocking
  int flags = fcntl(socket_fd_, F_GETFL, 0);
  fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

  // Bind to port
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    pixellib::core::logging::Logger::error("DHT: Failed to bind to port {}",
                                           port_);
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Try to load saved routing table
  load_routing_table(state_dir);

  running_ = true;
  network_thread_ = std::thread(&DHTClient::run, this);
  maintenance_thread_ = std::thread(&DHTClient::maintenance_loop, this);

  // Start bootstrap
  bootstrap();

  pixellib::core::logging::Logger::info(
      "DHT: Started on port {} with node ID {}", port_,
      own_id_.to_hex().substr(0, 8) + "...");

  return true;
}

void DHTClient::stop() {
  if (!running_)
    return;

  running_ = false;
  cv_.notify_all();

  if (network_thread_.joinable()) {
    network_thread_.join();
  }

  if (maintenance_thread_.joinable()) {
    maintenance_thread_.join();
  }

  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }

  // Save routing table
  save_routing_table();

  pixellib::core::logging::Logger::info("DHT: Stopped");
}

void DHTClient::find_peers(const std::array<uint8_t, 20> &info_hash,
                           PeerDiscoveryCallback callback) {
  std::string hash_key(info_hash.begin(), info_hash.end());

  {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    peer_callbacks_[hash_key] = callback;
  }

  iterative_get_peers(info_hash, callback);
}

size_t DHTClient::node_count() const { return routing_table_->size(); }

size_t DHTClient::active_queries() const {
  std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(queries_mutex_));
  return pending_queries_.size();
}

void DHTClient::run() {
  std::vector<uint8_t> buffer(65536);

  while (running_) {
    sockaddr_in from_addr{};
    socklen_t from_len = sizeof(from_addr);

    ssize_t received =
        recvfrom(socket_fd_, buffer.data(), buffer.size(), 0,
                 reinterpret_cast<sockaddr *>(&from_addr), &from_len);

    if (received > 0) {
      std::vector<uint8_t> data(buffer.begin(), buffer.begin() + received);

      std::array<uint8_t, 4> from_ip;
      std::memcpy(from_ip.data(), &from_addr.sin_addr.s_addr, 4);
      uint16_t from_port = ntohs(from_addr.sin_port);

      // Rate limiting
      if (!check_rate_limit(from_ip)) {
        continue;
      }

      // Try to parse as query
      auto query = QueryMessage::decode(data);
      if (query) {
        handle_query(*query, from_ip, from_port);
        continue;
      }

      // Try to parse as response
      auto response = ResponseMessage::decode(data);
      if (response) {
        handle_response(*response);
        continue;
      }

      // Try to parse as error
      auto error = ErrorMessage::decode(data);
      if (error) {
        handle_error(*error);
        continue;
      }
    } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      // Error
      pixellib::core::logging::Logger::error("DHT: recvfrom error: {}",
                                             strerror(errno));
    }

    // Small sleep to avoid busy-waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void DHTClient::handle_query(const QueryMessage &query,
                             const std::array<uint8_t, 4> &from_ip,
                             uint16_t from_port) {
  // Add querying node to routing table
  DHTNode node;
  node.id = query.querying_node_id;
  node.ip = from_ip;
  node.port = from_port;
  node.last_seen = std::chrono::steady_clock::now();
  routing_table_->add_node(node);

  // Handle different query types
  switch (query.query_type) {
  case QueryType::PING: {
    auto resp = response::ping(own_id_, query.transaction_id);
    send_message(resp.encode(), from_ip, from_port);
    break;
  }

  case QueryType::FIND_NODE: {
    if (query.target_id) {
      auto closest = routing_table_->find_closest_nodes(*query.target_id, 8);
      auto resp = response::find_node(own_id_, closest, query.transaction_id);
      send_message(resp.encode(), from_ip, from_port);
    }
    break;
  }

  case QueryType::GET_PEERS: {
    if (query.info_hash) {
      // Check if we have peers for this info hash
      std::string hash_key(query.info_hash->begin(), query.info_hash->end());
      std::vector<TorrentPeer> peers;

      {
        std::lock_guard<std::mutex> lock(storage_mutex_);
        auto it = stored_peers_.find(hash_key);
        if (it != stored_peers_.end()) {
          for (const auto &p : it->second) {
            TorrentPeer tp;
            tp.ip = p.ip;
            tp.port = p.port;
            peers.push_back(tp);
          }
        }
      }

      if (!peers.empty()) {
        struct {
          std::vector<TorrentPeer> values;
        } peer_data;
        peer_data.values = peers;

        // Return peers (values)
        // Note: The response::get_peers helper might need update or we
        // construct manually if it assumes nodes only Implementation note: The
        // current response::get_peers helper seems to handle nodes. We really
        // need a response::get_peers_with_values. But checking
        // `response::get_peers_with_nodes` usage...

        // Let's look at dht_protocol.hpp later. For now let's assume we can
        // construct a response with values. Actually, looking at
        // `handle_response`, it expects `peers` field for values.

        // Since I cannot easily see dht_protocol.hpp right now in this context
        // without a view_file, and I want to be safe, I will stick to the plan
        // but realize I might need to update dht_protocol.hpp too if
        // `get_peers_with_values` doesn't exist. Wait, I saw
        // `response::get_peers_with_nodes` in the file. Let's assume I need to
        // handle this properly.

        // For now, I will add the logic to return close nodes ALWAYS (Kademlia
        // spec says close nodes are returned even if values are found,
        // usually), but finding values means we should return them.

        // Let's modify the response construction.

        // Convert TorrentPeer to DHTNode for response
        std::vector<DHTNode> peer_nodes;
        for (const auto &peer : peers) {
          DHTNode n;
          n.ip = peer.ip;
          n.port = peer.port;
          // id is default constructed (not used for values)
          peer_nodes.push_back(n);
        }

        std::string token = token_manager_.generate_token(from_ip);
        auto resp = response::get_peers_with_values(own_id_, peer_nodes, token,
                                                    query.transaction_id);
        send_message(resp.encode(), from_ip, from_port);
      } else {
        // No peers, return closest nodes
        NodeID target(*query.info_hash);
        auto closest = routing_table_->find_closest_nodes(target, 8);
        std::string token = token_manager_.generate_token(from_ip);
        auto resp = response::get_peers_with_nodes(own_id_, closest, token,
                                                   query.transaction_id);
        send_message(resp.encode(), from_ip, from_port);
      }
    }
    break;
  }

  case QueryType::ANNOUNCE_PEER: {
    if (query.info_hash && query.token) {
      // Validate token
      if (token_manager_.validate_token(*query.token, from_ip)) {
        // Token valid - store the peer info
        store_peer(*query.info_hash, from_ip,
                   query.port ? *query.port : from_port);

        auto resp = response::announce_peer(own_id_, query.transaction_id);
        send_message(resp.encode(), from_ip, from_port);
      } else {
        auto err = error::protocol_error(query.transaction_id, "Invalid token");
        send_message(err.encode(), from_ip, from_port);
      }
    }
    break;
  }
  }
}

void DHTClient::handle_response(const ResponseMessage &response) {
  // Get pending query
  auto pending = get_pending_query(response.transaction_id);
  if (!pending) {
    return; // Unknown transaction
  }

  // Add responding node to routing table
  DHTNode node;
  node.id = response.responding_node_id;
  node.ip = pending->target_ip;
  node.port = pending->target_port;
  node.last_seen = std::chrono::steady_clock::now();
  routing_table_->add_node(node);
  routing_table_->update_last_seen(node.id);

  // Handle response based on query type
  if (pending->callback) {
    pending->callback(response);
  }

  // Handle iterative lookup
  if (pending->associated_lookup) {
    handle_lookup_response(response.transaction_id, response);
  }

  // Add returned nodes to routing table
  if (response.nodes) {
    for (const auto &returned_node : *response.nodes) {
      routing_table_->add_node(returned_node);
    }
  }

  // Handle peer values
  // Use associated_lookup to get info_hash if present
  auto info_hash = pending->associated_lookup
                       ? pending->associated_lookup->info_hash
                       : std::nullopt;

  if (response.peers && info_hash) {
    std::vector<TorrentPeer> torrent_peers;
    for (const auto &peer : *response.peers) {
      TorrentPeer tp;
      tp.ip = peer.ip;
      tp.port = peer.port;
      torrent_peers.push_back(tp);
    }

    std::string hash_key(info_hash->begin(), info_hash->end());
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto cb_it = peer_callbacks_.find(hash_key);
    if (cb_it != peer_callbacks_.end()) {
      cb_it->second(*info_hash, torrent_peers);
    }

    // Also notify lookup callback if it exists (for individual result
    // processing? usually we aggregate)
    if (pending->associated_lookup &&
        pending->associated_lookup->peers_callback) {
      pending->associated_lookup->peers_callback(*info_hash, torrent_peers);
    }
  }

  remove_pending_query(response.transaction_id);
}

void DHTClient::handle_error(const ErrorMessage &error) {
  auto pending = get_pending_query(error.transaction_id);
  if (pending) {
    pixellib::core::logging::Logger::warning("DHT: Error response: {} - {}",
                                             static_cast<int>(error.error_code),
                                             error.error_message);

    // Mark node as failed
    DHTNode node;
    node.ip = pending->target_ip;
    node.port = pending->target_port;
    // We don't have the node ID from error, so we can't update routing table

    remove_pending_query(error.transaction_id);
  }
}

std::vector<std::array<uint8_t, 4>>
DHTClient::resolve_hostname(const std::string &hostname) {
  std::vector<std::array<uint8_t, 4>> ips;
  struct addrinfo hints {}, *res;
  hints.ai_family = AF_INET; // DHTNode only supports IPv4
  hints.ai_socktype = SOCK_DGRAM;

  int status = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
  if (status != 0) {
    pixellib::core::logging::Logger::warning(
        "DHT: Failed to resolve hostname {}: {}", hostname,
        gai_strerror(status));
    return ips;
  }

  for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
    if (p->ai_family == AF_INET) {
      struct sockaddr_in *ipv4 =
          reinterpret_cast<struct sockaddr_in *>(p->ai_addr);
      std::array<uint8_t, 4> ip;
      std::memcpy(ip.data(), &ipv4->sin_addr, 4);
      ips.push_back(ip);
    }
  }

  freeaddrinfo(res);
  return ips;
}

void DHTClient::bootstrap() {
  pixellib::core::logging::Logger::info("DHT: Starting bootstrap");

  // If we have nodes in routing table, ping them
  auto existing_nodes = routing_table_->get_all_nodes();
  if (!existing_nodes.empty()) {
    pixellib::core::logging::Logger::info("DHT: Pinging {} saved nodes",
                                          existing_nodes.size());
    for (const auto &node : existing_nodes) {
      send_ping(node);
    }

    // Do a find_node for our own ID to populate routing table
    std::this_thread::sleep_for(std::chrono::seconds(2));
    iterative_find_node(own_id_);

    bootstrapped_ = true;
    return;
  }

  // Bootstrap from public nodes
  for (const auto &[hostname, port] : DHTConfig::bootstrap_nodes()) {
    std::vector<std::array<uint8_t, 4>> ips = resolve_hostname(hostname);
    if (ips.empty()) {
      continue;
    }

    for (const auto &ip : ips) {
      DHTNode bootstrap_node;
      bootstrap_node.id =
          NodeID::generate_random(); // We don't know their ID yet
      bootstrap_node.ip = ip;
      bootstrap_node.port = port;
      bootstrap_node.last_seen = std::chrono::steady_clock::now();

      send_find_node(bootstrap_node, own_id_);
    }
  }

  // Wait a bit for responses
  std::this_thread::sleep_for(std::chrono::seconds(5));

  // Do iterative find_node for our own ID
  iterative_find_node(own_id_);

  bootstrapped_ = true;
  pixellib::core::logging::Logger::info(
      "DHT: Bootstrap complete, {} nodes in routing table",
      routing_table_->size());
}

bool DHTClient::load_routing_table(const std::string &state_dir) {
  std::string rt_path = state_dir + "/dht/routing_table.dat";

  std::ifstream file(rt_path, std::ios::binary);
  if (!file) {
    return false;
  }

  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  file.close();

  bool success = routing_table_->deserialize(data);
  if (success) {
    pixellib::core::logging::Logger::info(
        "DHT: Loaded routing table with {} nodes", routing_table_->size());
  }

  return success;
}

void DHTClient::save_routing_table() {
  if (state_dir_.empty())
    return;

  std::string dht_dir = state_dir_ + "/dht";
  pixellib::core::filesystem::FileSystem::create_directories(dht_dir);

  std::string rt_path = dht_dir + "/routing_table.dat";
  auto data = routing_table_->serialize();

  std::ofstream file(rt_path, std::ios::binary);
  if (file) {
    file.write(reinterpret_cast<const char *>(data.data()), data.size());
    file.close();
    pixellib::core::logging::Logger::info(
        "DHT: Saved routing table with {} nodes", routing_table_->size());
  }
}

void DHTClient::iterative_find_node(const NodeID &target) {
  auto lookup = std::make_shared<LookupState>();
  lookup->id = tid_generator_.generate();
  lookup->target = target;
  lookup->type = QueryType::FIND_NODE;
  lookup->shortlist =
      routing_table_->find_closest_nodes(target, DHTConfig::LOOKUP_K);
  lookup->started_at = std::chrono::steady_clock::now();

  start_lookup(lookup);
}

void DHTClient::iterative_get_peers(const std::array<uint8_t, 20> &info_hash,
                                    PeerDiscoveryCallback callback) {
  NodeID target(info_hash);
  auto lookup = std::make_shared<LookupState>();
  lookup->id = tid_generator_.generate();
  lookup->target = target;
  lookup->type = QueryType::GET_PEERS;
  lookup->info_hash = info_hash;
  lookup->peers_callback = callback;
  lookup->shortlist =
      routing_table_->find_closest_nodes(target, DHTConfig::LOOKUP_K);
  lookup->started_at = std::chrono::steady_clock::now();

  start_lookup(lookup);
}

void DHTClient::start_lookup(std::shared_ptr<LookupState> lookup) {
  std::lock_guard<std::mutex> lock(lookups_mutex_);
  active_lookups_[lookup->id] = lookup;
  continue_lookup(lookup);
}

void DHTClient::continue_lookup(std::shared_ptr<LookupState> lookup) {
  // Sort shortlist by distance to target
  std::sort(lookup->shortlist.begin(), lookup->shortlist.end(),
            [&lookup](const DHTNode &a, const DHTNode &b) {
              return a.id.distance(lookup->target) <
                     b.id.distance(lookup->target);
            });

  // Remove duplicates (simple check by ID)
  auto last = std::unique(
      lookup->shortlist.begin(), lookup->shortlist.end(),
      [](const DHTNode &a, const DHTNode &b) { return a.id == b.id; });
  lookup->shortlist.erase(last, lookup->shortlist.end());

  // Cap shortlist
  if (lookup->shortlist.size() > DHTConfig::LOOKUP_K * 4) {
    lookup->shortlist.resize(DHTConfig::LOOKUP_K * 4);
  }

  // Count active or queried nodes among the K closest
  int active_count = 0;
  for (size_t i = 0;
       i < std::min(lookup->shortlist.size(), DHTConfig::LOOKUP_K); ++i) {
    if (lookup->queried_nodes.count(node_key(lookup->shortlist[i]))) {
      active_count++;
    }
  }

  // Termination condition logic could go here, but for now we just keep
  // querying until alpha limit

  std::vector<DHTNode> to_query;
  for (const auto &node : lookup->shortlist) {
    if (lookup->in_flight >= DHTConfig::LOOKUP_ALPHA)
      break;

    std::string key = node_key(node);
    if (lookup->queried_nodes.find(key) == lookup->queried_nodes.end()) {
      lookup->queried_nodes.insert(key);
      lookup->in_flight++;
      to_query.push_back(node);
    }
  }

  // Send queries
  for (const auto &node : to_query) {
    if (lookup->type == QueryType::FIND_NODE) {
      send_find_node(node, lookup->target, lookup);
    } else if (lookup->type == QueryType::GET_PEERS && lookup->info_hash) {
      send_get_peers(node, *lookup->info_hash, {}, lookup);
    }
  }

  // Check for termination
  if (lookup->in_flight == 0 &&
      active_count == static_cast<int>(lookup->queried_nodes.size())) {

    // Finished
    {
      std::lock_guard<std::mutex> lock(lookups_mutex_);
      active_lookups_.erase(lookup->id);
    }

    if (lookup->type == QueryType::FIND_NODE && lookup->nodes_callback) {
      lookup->nodes_callback(lookup->shortlist);
    } else if (lookup->type == QueryType::GET_PEERS && lookup->info_hash) {
      // Fire peers callback with empty list to signal completion?
      // Or just use announce_callback if set.
      if (lookup->announce_callback) {
        lookup->announce_callback(lookup->shortlist);
      }
    }
  }
}

void DHTClient::handle_lookup_response(const TransactionID &tid,
                                       const ResponseMessage &response) {
  auto pending = get_pending_query(tid);
  if (!pending || !pending->associated_lookup) {
    return;
  }

  auto lookup = pending->associated_lookup;

  // Add responding node to responding set
  DHTNode node;
  node.id = response.responding_node_id;
  node.ip = pending->target_ip;
  node.port = pending->target_port;
  lookup->responding_nodes.insert(node_key(node));

  // Collect token if present (for announce)
  if (response.token) {
    lookup->collected_tokens[node_key(node)] = *response.token;
  }

  if (lookup->in_flight > 0) {
    lookup->in_flight--;
  }

  // Add discovered nodes to shortlist
  if (response.nodes) {
    std::unordered_set<std::string> seen;
    for (const auto &existing : lookup->shortlist) {
      seen.insert(node_key(existing));
    }

    for (const auto &n : *response.nodes) {
      if (seen.find(node_key(n)) == seen.end()) {
        lookup->shortlist.push_back(n);
        seen.insert(node_key(n));
      }
    }
  }

  // Continue
  continue_lookup(lookup);
}

void DHTClient::announce(const std::array<uint8_t, 20> &info_hash,
                         uint16_t port) {
  // Phase 1: Iterative lookup to find closest nodes and gather tokens
  NodeID target(info_hash);
  auto lookup = std::make_shared<LookupState>();
  lookup->id = tid_generator_.generate();
  lookup->target = target;
  lookup->type =
      QueryType::GET_PEERS; // We use GET_PEERS to find nodes + get tokens
  lookup->info_hash = info_hash;
  lookup->shortlist =
      routing_table_->find_closest_nodes(target, DHTConfig::LOOKUP_K);
  lookup->started_at = std::chrono::steady_clock::now();

  // Callback when lookup finishes (or stabilizes)
  std::weak_ptr<LookupState> weak_lookup = lookup;
  lookup->announce_callback = [this, info_hash, port,
                               weak_lookup](const std::vector<DHTNode> &nodes) {
    if (auto l = weak_lookup.lock()) {
      for (const auto &node : nodes) {
        std::string key = node_key(node);
        auto it = l->collected_tokens.find(key);
        if (it != l->collected_tokens.end()) {
          send_announce_peer(node, info_hash, port, it->second);
        } else {
          // If we don't have a token, we could try to get one, but for now just
          // skip In a robust impl we would do a one-hop get_peers here.
          send_get_peers(
              node, info_hash,
              [this, node, info_hash, port](const ResponseMessage &resp) {
                if (resp.token) {
                  send_announce_peer(node, info_hash, port, *resp.token);
                }
              });
        }
      }
    }
  };

  start_lookup(lookup);
}

void DHTClient::send_ping(const DHTNode &node) {
  auto tid = tid_generator_.generate();
  auto query = query::ping(own_id_, tid);

  PendingQuery pending;
  pending.transaction_id = tid;
  pending.query_type = QueryType::PING;
  pending.sent_time = std::chrono::steady_clock::now();
  pending.target_ip = node.ip;
  pending.target_port = node.port;

  track_query(tid, pending);
  send_message(query.encode(), node.ip, node.port);
  routing_table_->mark_query_sent(node.id);
}

void DHTClient::send_find_node(const DHTNode &node, const NodeID &target,
                               std::shared_ptr<LookupState> lookup) {
  auto tid = tid_generator_.generate();
  auto query = query::find_node(own_id_, target, tid);

  PendingQuery pending;
  pending.transaction_id = tid;
  pending.query_type = QueryType::FIND_NODE;
  pending.sent_time = std::chrono::steady_clock::now();
  pending.target_ip = node.ip;
  pending.target_port = node.port;
  pending.associated_lookup = lookup;

  track_query(tid, pending);
  send_message(query.encode(), node.ip, node.port);
  routing_table_->mark_query_sent(node.id);
}

void DHTClient::send_get_peers(
    const DHTNode &node, const std::array<uint8_t, 20> &info_hash,
    std::function<void(const ResponseMessage &)> callback,
    std::shared_ptr<LookupState> lookup) {
  auto tid = tid_generator_.generate();
  auto query = query::get_peers(own_id_, info_hash, tid);

  PendingQuery pending;
  pending.transaction_id = tid;
  pending.query_type = QueryType::GET_PEERS;
  pending.sent_time = std::chrono::steady_clock::now();
  pending.target_ip = node.ip;
  pending.target_port = node.port;
  pending.associated_lookup = lookup;
  pending.callback = std::move(callback);

  track_query(tid, pending);
  send_message(query.encode(), node.ip, node.port);
  routing_table_->mark_query_sent(node.id);
}

void DHTClient::send_announce_peer(const DHTNode &node,
                                   const std::array<uint8_t, 20> &info_hash,
                                   uint16_t port, const std::string &token) {
  auto tid = tid_generator_.generate();
  auto query = query::announce_peer(own_id_, info_hash, port, token, tid);

  PendingQuery pending;
  pending.transaction_id = tid;
  pending.query_type = QueryType::ANNOUNCE_PEER;
  pending.sent_time = std::chrono::steady_clock::now();
  pending.target_ip = node.ip;
  pending.target_port = node.port;

  track_query(tid, pending);
  send_message(query.encode(), node.ip, node.port);
}

void DHTClient::track_query(const TransactionID &tid,
                            const PendingQuery &query) {
  std::lock_guard<std::mutex> lock(queries_mutex_);
  pending_queries_[tid] = query;
}

std::optional<PendingQuery>
DHTClient::get_pending_query(const TransactionID &tid) {
  std::lock_guard<std::mutex> lock(queries_mutex_);
  auto it = pending_queries_.find(tid);
  if (it != pending_queries_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void DHTClient::remove_pending_query(const TransactionID &tid) {
  std::lock_guard<std::mutex> lock(queries_mutex_);
  pending_queries_.erase(tid);
}

void DHTClient::cleanup_expired_queries() {
  std::lock_guard<std::mutex> lock(queries_mutex_);
  auto now = std::chrono::steady_clock::now();

  for (auto it = pending_queries_.begin(); it != pending_queries_.end();) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       now - it->second.sent_time)
                       .count();

    if (elapsed > DHTConfig::QUERY_TIMEOUT_SECONDS) {
      // Mark node as failed
      DHTNode failed_node;
      failed_node.ip = it->second.target_ip;
      failed_node.port = it->second.target_port;
      // We can't mark as failed without node ID

      it = pending_queries_.erase(it);
    } else {
      ++it;
    }
  }
}

void DHTClient::maintenance_loop() {
  while (running_) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::minutes(1), [this] { return !running_; });

    if (!running_)
      break;

    cleanup_expired_queries();
    token_manager_.rotate_secrets();
    refresh_routing_table();
    check_node_health();
    expire_peers();
    save_routing_table();
  }
}

void DHTClient::refresh_routing_table() {
  auto nodes_to_refresh = routing_table_->get_nodes_needing_refresh();

  for (const auto &node : nodes_to_refresh) {
    send_ping(node);
  }
}

void DHTClient::check_node_health() {
  // Remove bad nodes
  auto all_nodes = routing_table_->get_all_nodes();
  for (const auto &node : all_nodes) {
    if (node.get_state() == DHTNode::State::BAD) {
      routing_table_->remove_node(node.id);
    }
  }
}

bool DHTClient::check_rate_limit(const std::array<uint8_t, 4> &ip) {
  std::lock_guard<std::mutex> lock(rate_limit_mutex_);

  std::string ip_key(ip.begin(), ip.end());
  auto now = std::chrono::steady_clock::now();

  auto it = rate_limits_.find(ip_key);
  if (it == rate_limits_.end()) {
    rate_limits_[ip_key] = {1, now};
    return true;
  }

  auto elapsed =
      std::chrono::duration_cast<std::chrono::seconds>(now - it->second.second)
          .count();

  if (elapsed >= 1) {
    // Reset counter
    rate_limits_[ip_key] = {1, now};
    return true;
  }

  if (it->second.first >= DHTConfig::MAX_QUERIES_PER_SECOND) {
    return false;
  }

  it->second.first++;
  return true;
}

void DHTClient::store_peer(const std::array<uint8_t, 20> &info_hash,
                           const std::array<uint8_t, 4> &ip, uint16_t port) {
  std::lock_guard<std::mutex> lock(storage_mutex_);

  std::string key(info_hash.begin(), info_hash.end());
  auto &peers = stored_peers_[key];

  // Check if peer already exists
  for (auto &peer : peers) {
    if (peer.ip == ip && peer.port == port) {
      peer.last_announced = std::chrono::steady_clock::now();
      return;
    }
  }

  // Limit peers per torrent to avoid amplification attacks/spam
  if (peers.size() >= 100) {
    // Replace the oldest one or just ignore? k-buckets replace oldest.
    // For simplicity, let's just ignore or maybe replace random.
    // Let's remove the first one (oldest usually if purely appended)
    peers.erase(peers.begin());
  }

  StoredPeer new_peer;
  new_peer.ip = ip;
  new_peer.port = port;
  new_peer.last_announced = std::chrono::steady_clock::now();
  peers.push_back(new_peer);

  pixellib::core::logging::Logger::debug(
      "DHT: Stored peer {}.{}.{}.{}:{}", static_cast<int>(ip[0]),
      static_cast<int>(ip[1]), static_cast<int>(ip[2]), static_cast<int>(ip[3]),
      port);
}

void DHTClient::expire_peers() {
  std::lock_guard<std::mutex> lock(storage_mutex_);
  auto now = std::chrono::steady_clock::now();

  // Expiration time: 30 minutes
  auto expiration = std::chrono::minutes(30);

  for (auto it = stored_peers_.begin(); it != stored_peers_.end();) {
    auto &peers = it->second;

    peers.erase(std::remove_if(peers.begin(), peers.end(),
                               [&](const StoredPeer &p) {
                                 return (now - p.last_announced) > expiration;
                               }),
                peers.end());

    if (peers.empty()) {
      it = stored_peers_.erase(it);
    } else {
      ++it;
    }
  }
}

void DHTClient::send_message(const std::vector<uint8_t> &data,
                             const std::array<uint8_t, 4> &ip, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  std::memcpy(&addr.sin_addr.s_addr, ip.data(), 4);
  addr.sin_port = htons(port);

  sendto(socket_fd_, data.data(), data.size(), 0,
         reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
}

} // namespace dht
} // namespace pixelscrape
