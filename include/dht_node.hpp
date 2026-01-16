#pragma once

#include <array>
#include <vector>
#include <string>
#include <chrono>
#include <optional>
#include <memory>
#include <mutex>

namespace pixelscrape {
namespace dht {

// 160-bit node identifier (same size as SHA1 hash)
class NodeID {
public:
    static constexpr size_t ID_SIZE = 20;
    
    NodeID();
    explicit NodeID(const std::array<uint8_t, ID_SIZE>& id);
    explicit NodeID(const std::string& hex_string);
    
    // Generate random node ID
    static NodeID generate_random();
    
    // XOR distance metric (closer = smaller distance)
    NodeID distance(const NodeID& other) const;
    
    // Comparison operators
    bool operator==(const NodeID& other) const;
    bool operator!=(const NodeID& other) const;
    bool operator<(const NodeID& other) const;
    bool operator>(const NodeID& other) const;
    
    // Get the bucket index for this distance (0-159)
    int bucket_index() const;
    
    // Serialization
    const std::array<uint8_t, ID_SIZE>& bytes() const { return id_; }
    std::string to_hex() const;
    
private:
    std::array<uint8_t, ID_SIZE> id_;
};

// Represents a DHT node
struct DHTNode {
    NodeID id;
    std::array<uint8_t, 4> ip;
    uint16_t port;
    std::chrono::steady_clock::time_point last_seen;
    std::chrono::steady_clock::time_point last_query;
    int failed_queries{0};
    
    // Node quality states
    enum class State {
        GOOD,        // Responded within last 15 minutes
        QUESTIONABLE, // No response in 15+ minutes but < 1 hour
        BAD          // No response in 1+ hour or multiple failures
    };
    
    State get_state() const;
    bool is_good() const { return get_state() == State::GOOD; }
};

// K-bucket: stores up to K nodes with similar distance
class KBucket {
public:
    static constexpr size_t K = 8; // Standard Kademlia k value
    
    KBucket() = default;
    
    // Add or update a node (returns true if added/updated)
    bool add_node(const DHTNode& node);
    
    // Remove a node
    void remove_node(const NodeID& id);
    
    // Get node by ID
    std::optional<DHTNode> get_node(const NodeID& id) const;
    
    // Get all nodes
    std::vector<DHTNode> get_nodes() const;
    
    // Get good nodes only
    std::vector<DHTNode> get_good_nodes() const;
    
    // Check if bucket is full
    bool is_full() const { return nodes_.size() >= K; }
    
    // Get size
    size_t size() const { return nodes_.size(); }
    
    // Update last seen time for a node
    void update_last_seen(const NodeID& id);
    
    // Mark query sent to node
    void mark_query_sent(const NodeID& id);
    
    // Mark query failed
    void mark_query_failed(const NodeID& id);
    
private:
    std::vector<DHTNode> nodes_;
    mutable std::mutex mutex_;
    
    // Try to replace bad nodes with new node
    bool try_replace_bad_node(const DHTNode& new_node);
};

// Routing table: 160 k-buckets (one for each bit of distance)
class RoutingTable {
public:
    explicit RoutingTable(const NodeID& own_id);
    
    // Add or update a node
    void add_node(const DHTNode& node);
    
    // Remove a node
    void remove_node(const NodeID& id);
    
    // Find the K closest nodes to a target ID
    std::vector<DHTNode> find_closest_nodes(const NodeID& target, size_t count = 8) const;
    
    // Get all nodes
    std::vector<DHTNode> get_all_nodes() const;
    
    // Get total node count
    size_t size() const;
    
    // Update last seen time for a node
    void update_last_seen(const NodeID& id);
    
    // Mark query sent to node
    void mark_query_sent(const NodeID& id);
    
    // Mark query failed
    void mark_query_failed(const NodeID& id);
    
    // Get nodes that need refresh (haven't been queried recently)
    std::vector<DHTNode> get_nodes_needing_refresh() const;
    
    // Serialize/deserialize for persistence
    std::vector<uint8_t> serialize() const;
    bool deserialize(const std::vector<uint8_t>& data);
    
    const NodeID& own_id() const { return own_id_; }
    
private:
    NodeID own_id_;
    std::array<KBucket, 160> buckets_;
    mutable std::mutex mutex_;
    
    // Get bucket index for a node
    int get_bucket_index(const NodeID& node_id) const;
};

} // namespace dht
} // namespace pixelscrape
