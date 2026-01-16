#pragma once

#include "dht_node.hpp"
#include <array>
#include <bencode_parser.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pixelscrape {
namespace dht {

// Transaction ID for matching queries and responses
using TransactionID = std::string;

// KRPC message types
enum class MessageType { QUERY, RESPONSE, ERROR };

// DHT query types
enum class QueryType { PING, FIND_NODE, GET_PEERS, ANNOUNCE_PEER };

// KRPC Error codes
enum class ErrorCode {
  GENERIC_ERROR = 201,
  SERVER_ERROR = 202,
  PROTOCOL_ERROR = 203,
  METHOD_UNKNOWN = 204
};

// Base KRPC message
struct KRPCMessage {
  MessageType type;
  TransactionID transaction_id;

  virtual ~KRPCMessage() = default;
  virtual std::vector<uint8_t> encode() const = 0;
};

// Query message
struct QueryMessage : public KRPCMessage {
  QueryType query_type;
  NodeID querying_node_id;

  // Query-specific arguments
  std::optional<NodeID> target_id; // For find_node, get_peers
  std::optional<std::array<uint8_t, 20>>
      info_hash;                    // For get_peers, announce_peer
  std::optional<uint16_t> port;     // For announce_peer
  std::optional<std::string> token; // For announce_peer
  std::optional<bool> implied_port; // For announce_peer

  std::vector<uint8_t> encode() const override;
  static std::optional<QueryMessage> decode(const std::vector<uint8_t> &data);
};

// Response message
struct ResponseMessage : public KRPCMessage {
  NodeID responding_node_id;

  // Response-specific values
  std::optional<std::vector<DHTNode>> nodes; // For find_node
  std::optional<std::vector<DHTNode>>
      peers;                        // For get_peers (as compact peer info)
  std::optional<std::string> token; // For get_peers

  std::vector<uint8_t> encode() const override;
  static std::optional<ResponseMessage>
  decode(const std::vector<uint8_t> &data);
};

// Error message
struct ErrorMessage : public KRPCMessage {
  ErrorCode error_code;
  std::string error_message;

  std::vector<uint8_t> encode() const override;
  static std::optional<ErrorMessage> decode(const std::vector<uint8_t> &data);
};

// Token manager for announce_peer validation
class TokenManager {
public:
  TokenManager();

  // Generate a token for a peer
  std::string generate_token(const std::array<uint8_t, 4> &ip);

  // Validate a token from a peer
  bool validate_token(const std::string &token,
                      const std::array<uint8_t, 4> &ip);

  // Rotate secrets periodically (call every 5 minutes)
  void rotate_secrets();

private:
  std::array<uint8_t, 20> current_secret_;
  std::array<uint8_t, 20> previous_secret_;
  std::chrono::steady_clock::time_point last_rotation_;

  std::string compute_token(const std::array<uint8_t, 4> &ip,
                            const std::array<uint8_t, 20> &secret);
};

// Transaction ID generator
class TransactionIDGenerator {
public:
  TransactionIDGenerator();

  // Generate a new unique transaction ID
  TransactionID generate();

private:
  uint32_t counter_;
  std::mutex mutex_;
};

// Helper functions for compact node/peer encoding
namespace compact {
// Encode nodes in compact format (26 bytes per node: 20 ID + 4 IP + 2 port)
std::vector<uint8_t> encode_nodes(const std::vector<DHTNode> &nodes);

// Decode nodes from compact format
std::vector<DHTNode> decode_nodes(const std::vector<uint8_t> &data);

// Encode peers in compact format (6 bytes per peer: 4 IP + 2 port)
std::vector<uint8_t> encode_peers(const std::vector<DHTNode> &peers);

// Decode peers from compact format (returns nodes with empty IDs)
std::vector<DHTNode> decode_peers(const std::vector<uint8_t> &data);
} // namespace compact

// Query builders
namespace query {
QueryMessage ping(const NodeID &own_id, const TransactionID &tid);

QueryMessage find_node(const NodeID &own_id, const NodeID &target,
                       const TransactionID &tid);

QueryMessage get_peers(const NodeID &own_id,
                       const std::array<uint8_t, 20> &info_hash,
                       const TransactionID &tid);

QueryMessage announce_peer(const NodeID &own_id,
                           const std::array<uint8_t, 20> &info_hash,
                           uint16_t port, const std::string &token,
                           const TransactionID &tid, bool implied_port = false);
} // namespace query

// Response builders
namespace response {
ResponseMessage ping(const NodeID &own_id, const TransactionID &tid);

ResponseMessage find_node(const NodeID &own_id,
                          const std::vector<DHTNode> &nodes,
                          const TransactionID &tid);

ResponseMessage get_peers_with_nodes(const NodeID &own_id,
                                     const std::vector<DHTNode> &nodes,
                                     const std::string &token,
                                     const TransactionID &tid);

ResponseMessage get_peers_with_values(const NodeID &own_id,
                                      const std::vector<DHTNode> &peers,
                                      const std::string &token,
                                      const TransactionID &tid);

ResponseMessage announce_peer(const NodeID &own_id, const TransactionID &tid);
} // namespace response

// Error builders
namespace error {
ErrorMessage generic_error(const TransactionID &tid,
                           const std::string &message);
ErrorMessage protocol_error(const TransactionID &tid,
                            const std::string &message);
ErrorMessage method_unknown(const TransactionID &tid);
} // namespace error

} // namespace dht
} // namespace pixelscrape
