#include "dht_node.hpp"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace pixelscrape {
namespace dht {

// NodeID implementation

NodeID::NodeID() { id_.fill(0); }

NodeID::NodeID(const std::array<uint8_t, ID_SIZE> &id) : id_(id) {}

NodeID::NodeID(const std::string &hex_string) {
  id_.fill(0);
  if (hex_string.size() != ID_SIZE * 2) {
    return; // Invalid hex string
  }

  for (size_t i = 0; i < ID_SIZE; ++i) {
    std::string byte_str = hex_string.substr(i * 2, 2);
    id_[i] = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
  }
}

NodeID NodeID::generate_random() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint8_t> dis(0, 255);

  std::array<uint8_t, ID_SIZE> id;
  for (auto &byte : id) {
    byte = dis(gen);
  }

  return NodeID(id);
}

NodeID NodeID::distance(const NodeID &other) const {
  std::array<uint8_t, ID_SIZE> result;
  for (size_t i = 0; i < ID_SIZE; ++i) {
    result[i] = id_[i] ^ other.id_[i];
  }
  return NodeID(result);
}

bool NodeID::operator==(const NodeID &other) const { return id_ == other.id_; }

bool NodeID::operator!=(const NodeID &other) const { return id_ != other.id_; }

bool NodeID::operator<(const NodeID &other) const { return id_ < other.id_; }

bool NodeID::operator>(const NodeID &other) const { return id_ > other.id_; }

int NodeID::bucket_index() const {
  // Find the first non-zero bit (most significant bit)
  for (size_t i = 0; i < ID_SIZE; ++i) {
    if (id_[i] != 0) {
      // Count leading zeros in this byte
      uint8_t byte = id_[i];
      int leading_zeros = 0;
      for (int bit = 7; bit >= 0; --bit) {
        if (byte & (1 << bit)) {
          break;
        }
        leading_zeros++;
      }
      return (159 - (i * 8 + leading_zeros));
    }
  }
  return 0; // All zeros (same node)
}

std::string NodeID::to_hex() const {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (uint8_t byte : id_) {
    oss << std::setw(2) << static_cast<int>(byte);
  }
  return oss.str();
}

// DHTNode implementation

DHTNode::State DHTNode::get_state() const {
  auto now = std::chrono::steady_clock::now();
  auto time_since_seen =
      std::chrono::duration_cast<std::chrono::minutes>(now - last_seen).count();

  if (failed_queries >= 3) {
    return State::BAD;
  }

  if (time_since_seen < 15) {
    return State::GOOD;
  } else if (time_since_seen < 60) {
    return State::QUESTIONABLE;
  } else {
    return State::BAD;
  }
}

// KBucket implementation

bool KBucket::add_node(const DHTNode &node) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if node already exists
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [&node](const DHTNode &n) { return n.id == node.id; });

  if (it != nodes_.end()) {
    // Update existing node
    *it = node;
    return true;
  }

  // Add new node if there's space
  if (nodes_.size() < K) {
    nodes_.push_back(node);
    return true;
  }

  // Bucket is full, try to replace a bad node
  return try_replace_bad_node(node);
}

void KBucket::remove_node(const NodeID &id) {
  std::lock_guard<std::mutex> lock(mutex_);
  nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                              [&id](const DHTNode &n) { return n.id == id; }),
               nodes_.end());
}

std::optional<DHTNode> KBucket::get_node(const NodeID &id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [&id](const DHTNode &n) { return n.id == id; });
  if (it != nodes_.end()) {
    return *it;
  }
  return std::nullopt;
}

std::vector<DHTNode> KBucket::get_nodes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return nodes_;
}

std::vector<DHTNode> KBucket::get_good_nodes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<DHTNode> good_nodes;
  std::copy_if(nodes_.begin(), nodes_.end(), std::back_inserter(good_nodes),
               [](const DHTNode &n) { return n.is_good(); });
  return good_nodes;
}

void KBucket::update_last_seen(const NodeID &id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [&id](const DHTNode &n) { return n.id == id; });
  if (it != nodes_.end()) {
    it->last_seen = std::chrono::steady_clock::now();
    it->failed_queries = 0; // Reset failure count on successful response
  }
}

void KBucket::mark_query_sent(const NodeID &id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [&id](const DHTNode &n) { return n.id == id; });
  if (it != nodes_.end()) {
    it->last_query = std::chrono::steady_clock::now();
  }
}

void KBucket::mark_query_failed(const NodeID &id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [&id](const DHTNode &n) { return n.id == id; });
  if (it != nodes_.end()) {
    it->failed_queries++;
  }
}

bool KBucket::try_replace_bad_node(const DHTNode &new_node) {
  // Find a bad node to replace
  auto it = std::find_if(nodes_.begin(), nodes_.end(), [](const DHTNode &n) {
    return n.get_state() == DHTNode::State::BAD;
  });

  if (it != nodes_.end()) {
    *it = new_node;
    return true;
  }

  return false;
}

// RoutingTable implementation

RoutingTable::RoutingTable(const NodeID &own_id) : own_id_(own_id) {}

void RoutingTable::add_node(const DHTNode &node) {
  if (node.id == own_id_) {
    return; // Don't add ourselves
  }

  int bucket_idx = get_bucket_index(node.id);
  if (bucket_idx >= 0 && bucket_idx < 160) {
    buckets_[bucket_idx].add_node(node);
  }
}

void RoutingTable::remove_node(const NodeID &id) {
  int bucket_idx = get_bucket_index(id);
  if (bucket_idx >= 0 && bucket_idx < 160) {
    buckets_[bucket_idx].remove_node(id);
  }
}

std::vector<DHTNode> RoutingTable::find_closest_nodes(const NodeID &target,
                                                      size_t count) const {
  std::lock_guard<std::mutex> lock(mutex_);

  // Collect all nodes
  std::vector<DHTNode> all_nodes;
  for (const auto &bucket : buckets_) {
    auto nodes = bucket.get_good_nodes();
    all_nodes.insert(all_nodes.end(), nodes.begin(), nodes.end());
  }

  // Sort by distance to target
  std::sort(all_nodes.begin(), all_nodes.end(),
            [&target](const DHTNode &a, const DHTNode &b) {
              return a.id.distance(target) < b.id.distance(target);
            });

  // Return up to 'count' closest nodes
  if (all_nodes.size() > count) {
    all_nodes.resize(count);
  }

  return all_nodes;
}

std::vector<DHTNode> RoutingTable::get_all_nodes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<DHTNode> all_nodes;
  for (const auto &bucket : buckets_) {
    auto nodes = bucket.get_nodes();
    all_nodes.insert(all_nodes.end(), nodes.begin(), nodes.end());
  }
  return all_nodes;
}

size_t RoutingTable::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t total = 0;
  for (const auto &bucket : buckets_) {
    total += bucket.size();
  }
  return total;
}

void RoutingTable::update_last_seen(const NodeID &id) {
  int bucket_idx = get_bucket_index(id);
  if (bucket_idx >= 0 && bucket_idx < 160) {
    buckets_[bucket_idx].update_last_seen(id);
  }
}

void RoutingTable::mark_query_sent(const NodeID &id) {
  int bucket_idx = get_bucket_index(id);
  if (bucket_idx >= 0 && bucket_idx < 160) {
    buckets_[bucket_idx].mark_query_sent(id);
  }
}

void RoutingTable::mark_query_failed(const NodeID &id) {
  int bucket_idx = get_bucket_index(id);
  if (bucket_idx >= 0 && bucket_idx < 160) {
    buckets_[bucket_idx].mark_query_failed(id);
  }
}

std::vector<DHTNode> RoutingTable::get_nodes_needing_refresh() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<DHTNode> nodes_to_refresh;
  auto now = std::chrono::steady_clock::now();

  for (const auto &bucket : buckets_) {
    auto nodes = bucket.get_nodes();
    for (const auto &node : nodes) {
      auto time_since_query = std::chrono::duration_cast<std::chrono::minutes>(
                                  now - node.last_query)
                                  .count();

      // Refresh nodes that haven't been queried in 15+ minutes
      if (time_since_query >= 15) {
        nodes_to_refresh.push_back(node);
      }
    }
  }

  return nodes_to_refresh;
}

std::vector<uint8_t> RoutingTable::serialize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<uint8_t> data;

  // Serialize own ID
  const auto &own_id_bytes = own_id_.bytes();
  data.insert(data.end(), own_id_bytes.begin(), own_id_bytes.end());

  // Get all good nodes
  std::vector<DHTNode> good_nodes;
  for (const auto &bucket : buckets_) {
    auto nodes = bucket.get_good_nodes();
    good_nodes.insert(good_nodes.end(), nodes.begin(), nodes.end());
  }

  // Write node count (4 bytes)
  uint32_t count = static_cast<uint32_t>(good_nodes.size());
  data.push_back((count >> 24) & 0xFF);
  data.push_back((count >> 16) & 0xFF);
  data.push_back((count >> 8) & 0xFF);
  data.push_back(count & 0xFF);

  // Write each node (26 bytes: 20 ID + 4 IP + 2 port)
  for (const auto &node : good_nodes) {
    const auto &id_bytes = node.id.bytes();
    data.insert(data.end(), id_bytes.begin(), id_bytes.end());
    data.insert(data.end(), node.ip.begin(), node.ip.end());
    data.push_back((node.port >> 8) & 0xFF);
    data.push_back(node.port & 0xFF);
  }

  return data;
}

bool RoutingTable::deserialize(const std::vector<uint8_t> &data) {
  if (data.size() < 24) { // At least own ID + count
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // Read own ID
  std::array<uint8_t, NodeID::ID_SIZE> own_id_bytes;
  std::copy(data.begin(), data.begin() + NodeID::ID_SIZE, own_id_bytes.begin());
  own_id_ = NodeID(own_id_bytes);

  // Read node count
  size_t offset = NodeID::ID_SIZE;
  uint32_t count = (static_cast<uint32_t>(data[offset]) << 24) |
                   (static_cast<uint32_t>(data[offset + 1]) << 16) |
                   (static_cast<uint32_t>(data[offset + 2]) << 8) |
                   static_cast<uint32_t>(data[offset + 3]);
  offset += 4;

  // Read each node
  for (uint32_t i = 0; i < count; ++i) {
    if (offset + 26 > data.size()) {
      break; // Corrupted data
    }

    std::array<uint8_t, NodeID::ID_SIZE> id_bytes;
    std::copy(data.begin() + offset, data.begin() + offset + NodeID::ID_SIZE,
              id_bytes.begin());
    offset += NodeID::ID_SIZE;

    DHTNode node;
    node.id = NodeID(id_bytes);
    std::copy(data.begin() + offset, data.begin() + offset + 4,
              node.ip.begin());
    offset += 4;

    node.port = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
    offset += 2;

    node.last_seen = std::chrono::steady_clock::now();
    node.last_query = std::chrono::steady_clock::now() -
                      std::chrono::minutes(20); // Needs refresh

    add_node(node);
  }

  return true;
}

int RoutingTable::get_bucket_index(const NodeID &node_id) const {
  NodeID dist = own_id_.distance(node_id);
  return dist.bucket_index();
}

} // namespace dht
} // namespace pixelscrape
