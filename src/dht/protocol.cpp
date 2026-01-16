#include "dht_protocol.hpp"
#include "sha1.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <logging.hpp>
#include <random>
#include <sstream>

namespace pixelscrape {
namespace dht {

// Helper to convert BencodeValue to string
static std::string bencode_to_string(const BencodeValue &value) {
  if (auto *str = std::get_if<BencodeString>(&value)) {
    return *str;
  }
  return "";
}

// Helper to convert BencodeValue to integer
static int64_t bencode_to_int(const BencodeValue &value) {
  if (auto *num = std::get_if<BencodeInteger>(&value)) {
    return *num;
  }
  return 0;
}

// QueryMessage implementation

std::vector<uint8_t> QueryMessage::encode() const {
  // Build bencode dictionary
  std::string query_name;
  switch (query_type) {
  case QueryType::PING:
    query_name = "ping";
    break;
  case QueryType::FIND_NODE:
    query_name = "find_node";
    break;
  case QueryType::GET_PEERS:
    query_name = "get_peers";
    break;
  case QueryType::ANNOUNCE_PEER:
    query_name = "announce_peer";
    break;
  }

  std::ostringstream oss;
  oss << "d"; // Start dictionary

  // Add "a" (arguments) dictionary
  oss << "1:ad";
  oss << "2:id20:";
  oss.write(reinterpret_cast<const char *>(querying_node_id.bytes().data()),
            20);

  if (target_id) {
    oss << "6:target20:";
    oss.write(reinterpret_cast<const char *>(target_id->bytes().data()), 20);
  }

  if (info_hash) {
    oss << "9:info_hash20:";
    oss.write(reinterpret_cast<const char *>(info_hash->data()), 20);
  }

  if (port) {
    oss << "4:porti" << *port << "e";
  }

  if (token) {
    oss << "5:token" << token->size() << ":" << *token;
  }

  if (implied_port && *implied_port) {
    oss << "12:implied_porti1e";
  }

  oss << "e"; // End arguments dictionary

  // Add "q" (query name)
  oss << "1:q" << query_name.size() << ":" << query_name;

  // Add "t" (transaction ID)
  oss << "1:t" << transaction_id.size() << ":" << transaction_id;

  // Add "y" (message type)
  oss << "1:y1:q";

  oss << "e"; // End main dictionary

  std::string encoded = oss.str();
  return std::vector<uint8_t>(encoded.begin(), encoded.end());
}

std::optional<QueryMessage>
QueryMessage::decode(const std::vector<uint8_t> &data) {
  try {
    std::string data_str(data.begin(), data.end());
    BencodeValue parsed = BencodeParser::parse(data_str);

    auto *dict = std::get_if<std::unique_ptr<BencodeDict>>(&parsed);
    if (!dict || !*dict)
      return std::nullopt;

    QueryMessage msg;
    msg.type = MessageType::QUERY;

    // Get transaction ID
    auto tid_it = (*dict)->values.find("t");
    if (tid_it != (*dict)->values.end()) {
      msg.transaction_id = bencode_to_string(tid_it->second);
    }

    // Get query type
    auto q_it = (*dict)->values.find("q");
    if (q_it != (*dict)->values.end()) {
      std::string query_name = bencode_to_string(q_it->second);
      if (query_name == "ping")
        msg.query_type = QueryType::PING;
      else if (query_name == "find_node")
        msg.query_type = QueryType::FIND_NODE;
      else if (query_name == "get_peers")
        msg.query_type = QueryType::GET_PEERS;
      else if (query_name == "announce_peer")
        msg.query_type = QueryType::ANNOUNCE_PEER;
      else
        return std::nullopt;
    }

    // Get arguments
    auto a_it = (*dict)->values.find("a");
    if (a_it != (*dict)->values.end()) {
      auto *args_dict =
          std::get_if<std::unique_ptr<BencodeDict>>(&a_it->second);
      if (args_dict && *args_dict) {
        // Get node ID
        auto id_it = (*args_dict)->values.find("id");
        if (id_it != (*args_dict)->values.end()) {
          std::string id_str = bencode_to_string(id_it->second);
          if (id_str.size() == 20) {
            std::array<uint8_t, 20> id_bytes;
            std::copy(id_str.begin(), id_str.end(), id_bytes.begin());
            msg.querying_node_id = NodeID(id_bytes);
          }
        }

        // Get target (for find_node)
        auto target_it = (*args_dict)->values.find("target");
        if (target_it != (*args_dict)->values.end()) {
          std::string target_str = bencode_to_string(target_it->second);
          if (target_str.size() == 20) {
            std::array<uint8_t, 20> target_bytes;
            std::copy(target_str.begin(), target_str.end(),
                      target_bytes.begin());
            msg.target_id = NodeID(target_bytes);
          }
        }

        // Get info_hash (for get_peers, announce_peer)
        auto ih_it = (*args_dict)->values.find("info_hash");
        if (ih_it != (*args_dict)->values.end()) {
          std::string ih_str = bencode_to_string(ih_it->second);
          if (ih_str.size() == 20) {
            std::array<uint8_t, 20> ih_bytes;
            std::copy(ih_str.begin(), ih_str.end(), ih_bytes.begin());
            msg.info_hash = ih_bytes;
          }
        }

        // Get port (for announce_peer)
        auto port_it = (*args_dict)->values.find("port");
        if (port_it != (*args_dict)->values.end()) {
          msg.port = static_cast<uint16_t>(bencode_to_int(port_it->second));
        }

        // Get token (for announce_peer)
        auto token_it = (*args_dict)->values.find("token");
        if (token_it != (*args_dict)->values.end()) {
          msg.token = bencode_to_string(token_it->second);
        }

        // Get implied_port
        auto imp_it = (*args_dict)->values.find("implied_port");
        if (imp_it != (*args_dict)->values.end()) {
          msg.implied_port = (bencode_to_int(imp_it->second) != 0);
        }
      }
    }

    return msg;
  } catch (...) {
    return std::nullopt;
  }
}

// ResponseMessage implementation

std::vector<uint8_t> ResponseMessage::encode() const {
  std::ostringstream oss;
  oss << "d"; // Start dictionary

  // Add "r" (response) dictionary
  oss << "1:rd";
  oss << "2:id20:";
  oss.write(reinterpret_cast<const char *>(responding_node_id.bytes().data()),
            20);

  if (nodes && !nodes->empty()) {
    auto compact_nodes = compact::encode_nodes(*nodes);
    oss << "5:nodes" << compact_nodes.size() << ":";
    oss.write(reinterpret_cast<const char *>(compact_nodes.data()),
              compact_nodes.size());
  }

  if (peers && !peers->empty()) {
    auto compact_peers = compact::encode_peers(*peers);
    oss << "6:valuesl";
    for (const auto &peer : *peers) {
      oss << "6:";
      oss.write(reinterpret_cast<const char *>(peer.ip.data()), 4);
      uint16_t port_be = htons(peer.port);
      oss.write(reinterpret_cast<const char *>(&port_be), 2);
    }
    oss << "e";
  }

  if (token) {
    oss << "5:token" << token->size() << ":" << *token;
  }

  oss << "e"; // End response dictionary

  // Add "t" (transaction ID)
  oss << "1:t" << transaction_id.size() << ":" << transaction_id;

  // Add "y" (message type)
  oss << "1:y1:r";

  oss << "e"; // End main dictionary

  std::string encoded = oss.str();
  return std::vector<uint8_t>(encoded.begin(), encoded.end());
}

std::optional<ResponseMessage>
ResponseMessage::decode(const std::vector<uint8_t> &data) {
  try {
    std::string data_str(data.begin(), data.end());
    BencodeValue parsed = BencodeParser::parse(data_str);

    auto *dict = std::get_if<std::unique_ptr<BencodeDict>>(&parsed);
    if (!dict || !*dict)
      return std::nullopt;

    ResponseMessage msg;
    msg.type = MessageType::RESPONSE;

    // Get transaction ID
    auto tid_it = (*dict)->values.find("t");
    if (tid_it != (*dict)->values.end()) {
      msg.transaction_id = bencode_to_string(tid_it->second);
    }

    // Get response values
    auto r_it = (*dict)->values.find("r");
    if (r_it != (*dict)->values.end()) {
      auto *resp_dict =
          std::get_if<std::unique_ptr<BencodeDict>>(&r_it->second);
      if (resp_dict && *resp_dict) {
        // Get node ID
        auto id_it = (*resp_dict)->values.find("id");
        if (id_it != (*resp_dict)->values.end()) {
          std::string id_str = bencode_to_string(id_it->second);
          if (id_str.size() == 20) {
            std::array<uint8_t, 20> id_bytes;
            std::copy(id_str.begin(), id_str.end(), id_bytes.begin());
            msg.responding_node_id = NodeID(id_bytes);
          }
        }

        // Get nodes (compact format)
        auto nodes_it = (*resp_dict)->values.find("nodes");
        if (nodes_it != (*resp_dict)->values.end()) {
          std::string nodes_str = bencode_to_string(nodes_it->second);
          std::vector<uint8_t> nodes_data(nodes_str.begin(), nodes_str.end());
          msg.nodes = compact::decode_nodes(nodes_data);
        }

        // Get token
        auto token_it = (*resp_dict)->values.find("token");
        if (token_it != (*resp_dict)->values.end()) {
          msg.token = bencode_to_string(token_it->second);
        }

        // Get values (peers)
        auto values_it = (*resp_dict)->values.find("values");
        if (values_it != (*resp_dict)->values.end()) {
          auto *values_list =
              std::get_if<std::unique_ptr<BencodeList>>(&values_it->second);
          if (values_list && *values_list) {
            std::vector<DHTNode> peers;
            for (const auto &value : (*values_list)->items) {
              std::string peer_str = bencode_to_string(value);
              if (peer_str.size() == 6) {
                DHTNode peer;
                std::copy(peer_str.begin(), peer_str.begin() + 4,
                          peer.ip.begin());
                uint16_t port_be;
                std::memcpy(&port_be, peer_str.data() + 4, 2);
                peer.port = ntohs(port_be);
                peers.push_back(peer);
              }
            }
            if (!peers.empty()) {
              msg.peers = peers;
            }
          }
        }
      }
    }

    return msg;
  } catch (...) {
    return std::nullopt;
  }
}

// ErrorMessage implementation

std::vector<uint8_t> ErrorMessage::encode() const {
  std::ostringstream oss;
  oss << "d";

  // Add "e" (error) list
  oss << "1:eli" << static_cast<int>(error_code) << "e";
  oss << error_message.size() << ":" << error_message;
  oss << "e";

  // Add "t" (transaction ID)
  oss << "1:t" << transaction_id.size() << ":" << transaction_id;

  // Add "y" (message type)
  oss << "1:y1:e";

  oss << "e";

  std::string encoded = oss.str();
  return std::vector<uint8_t>(encoded.begin(), encoded.end());
}

std::optional<ErrorMessage>
ErrorMessage::decode(const std::vector<uint8_t> &data) {
  try {
    std::string data_str(data.begin(), data.end());
    BencodeValue parsed = BencodeParser::parse(data_str);

    auto *dict = std::get_if<std::unique_ptr<BencodeDict>>(&parsed);
    if (!dict || !*dict)
      return std::nullopt;

    ErrorMessage msg;
    msg.type = MessageType::ERROR;

    // Get transaction ID
    auto tid_it = (*dict)->values.find("t");
    if (tid_it != (*dict)->values.end()) {
      msg.transaction_id = bencode_to_string(tid_it->second);
    }

    // Get error
    auto e_it = (*dict)->values.find("e");
    if (e_it != (*dict)->values.end()) {
      auto *error_list =
          std::get_if<std::unique_ptr<BencodeList>>(&e_it->second);
      if (error_list && *error_list && (*error_list)->items.size() >= 2) {
        msg.error_code =
            static_cast<ErrorCode>(bencode_to_int((*error_list)->items[0]));
        msg.error_message = bencode_to_string((*error_list)->items[1]);
      }
    }

    return msg;
  } catch (...) {
    return std::nullopt;
  }
}

// TokenManager implementation

TokenManager::TokenManager() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint8_t> dis(0, 255);

  for (auto &byte : current_secret_)
    byte = dis(gen);
  for (auto &byte : previous_secret_)
    byte = dis(gen);

  last_rotation_ = std::chrono::steady_clock::now();
}

std::string TokenManager::generate_token(const std::array<uint8_t, 4> &ip) {
  return compute_token(ip, current_secret_);
}

bool TokenManager::validate_token(const std::string &token,
                                  const std::array<uint8_t, 4> &ip) {
  std::string current_token = compute_token(ip, current_secret_);
  if (token == current_token)
    return true;

  std::string previous_token = compute_token(ip, previous_secret_);
  return token == previous_token;
}

void TokenManager::rotate_secrets() {
  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::minutes>(now - last_rotation_)
          .count();

  if (elapsed >= 5) {
    previous_secret_ = current_secret_;

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint8_t> dis(0, 255);
    for (auto &byte : current_secret_)
      byte = dis(gen);

    last_rotation_ = now;
  }
}

std::string TokenManager::compute_token(const std::array<uint8_t, 4> &ip,
                                        const std::array<uint8_t, 20> &secret) {
  // Simple token: SHA1(IP + secret)
  std::vector<uint8_t> data;
  data.insert(data.end(), ip.begin(), ip.end());
  data.insert(data.end(), secret.begin(), secret.end());

  auto hash = pixelscrape::SHA1::hash(data);
  return std::string(hash.begin(), hash.end());
}

// TransactionIDGenerator implementation

TransactionIDGenerator::TransactionIDGenerator() : counter_(0) {}

TransactionID TransactionIDGenerator::generate() {
  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t id = counter_++;

  std::array<uint8_t, 4> bytes;
  bytes[0] = (id >> 24) & 0xFF;
  bytes[1] = (id >> 16) & 0xFF;
  bytes[2] = (id >> 8) & 0xFF;
  bytes[3] = id & 0xFF;

  return std::string(bytes.begin(), bytes.end());
}

// Compact encoding helpers

namespace compact {

std::vector<uint8_t> encode_nodes(const std::vector<DHTNode> &nodes) {
  std::vector<uint8_t> data;
  data.reserve(nodes.size() * 26);

  for (const auto &node : nodes) {
    const auto &id_bytes = node.id.bytes();
    data.insert(data.end(), id_bytes.begin(), id_bytes.end());
    data.insert(data.end(), node.ip.begin(), node.ip.end());
    uint16_t port_be = htons(node.port);
    data.push_back((port_be >> 8) & 0xFF);
    data.push_back(port_be & 0xFF);
  }

  return data;
}

std::vector<DHTNode> decode_nodes(const std::vector<uint8_t> &data) {
  std::vector<DHTNode> nodes;

  for (size_t i = 0; i + 26 <= data.size(); i += 26) {
    DHTNode node;

    std::array<uint8_t, 20> id_bytes;
    std::copy(data.begin() + i, data.begin() + i + 20, id_bytes.begin());
    node.id = NodeID(id_bytes);

    std::copy(data.begin() + i + 20, data.begin() + i + 24, node.ip.begin());

    uint16_t port_be =
        (static_cast<uint16_t>(data[i + 24]) << 8) | data[i + 25];
    node.port = ntohs(port_be);

    node.last_seen = std::chrono::steady_clock::now();
    node.last_query =
        std::chrono::steady_clock::now() - std::chrono::minutes(20);

    nodes.push_back(node);
  }

  return nodes;
}

std::vector<uint8_t> encode_peers(const std::vector<DHTNode> &peers) {
  std::vector<uint8_t> data;
  data.reserve(peers.size() * 6);

  for (const auto &peer : peers) {
    data.insert(data.end(), peer.ip.begin(), peer.ip.end());
    uint16_t port_be = htons(peer.port);
    data.push_back((port_be >> 8) & 0xFF);
    data.push_back(port_be & 0xFF);
  }

  return data;
}

std::vector<DHTNode> decode_peers(const std::vector<uint8_t> &data) {
  std::vector<DHTNode> peers;

  for (size_t i = 0; i + 6 <= data.size(); i += 6) {
    DHTNode peer;

    std::copy(data.begin() + i, data.begin() + i + 4, peer.ip.begin());

    uint16_t port_be = (static_cast<uint16_t>(data[i + 4]) << 8) | data[i + 5];
    peer.port = ntohs(port_be);

    peer.last_seen = std::chrono::steady_clock::now();

    peers.push_back(peer);
  }

  return peers;
}

} // namespace compact

// Query builders

namespace query {

QueryMessage ping(const NodeID &own_id, const TransactionID &tid) {
  QueryMessage msg;
  msg.type = MessageType::QUERY;
  msg.query_type = QueryType::PING;
  msg.transaction_id = tid;
  msg.querying_node_id = own_id;
  return msg;
}

QueryMessage find_node(const NodeID &own_id, const NodeID &target,
                       const TransactionID &tid) {
  QueryMessage msg;
  msg.type = MessageType::QUERY;
  msg.query_type = QueryType::FIND_NODE;
  msg.transaction_id = tid;
  msg.querying_node_id = own_id;
  msg.target_id = target;
  return msg;
}

QueryMessage get_peers(const NodeID &own_id,
                       const std::array<uint8_t, 20> &info_hash,
                       const TransactionID &tid) {
  QueryMessage msg;
  msg.type = MessageType::QUERY;
  msg.query_type = QueryType::GET_PEERS;
  msg.transaction_id = tid;
  msg.querying_node_id = own_id;
  msg.info_hash = info_hash;
  return msg;
}

QueryMessage announce_peer(const NodeID &own_id,
                           const std::array<uint8_t, 20> &info_hash,
                           uint16_t port, const std::string &token,
                           const TransactionID &tid, bool implied_port) {
  QueryMessage msg;
  msg.type = MessageType::QUERY;
  msg.query_type = QueryType::ANNOUNCE_PEER;
  msg.transaction_id = tid;
  msg.querying_node_id = own_id;
  msg.info_hash = info_hash;
  msg.port = port;
  msg.token = token;
  msg.implied_port = implied_port;
  return msg;
}

} // namespace query

// Response builders

namespace response {

ResponseMessage ping(const NodeID &own_id, const TransactionID &tid) {
  ResponseMessage msg;
  msg.type = MessageType::RESPONSE;
  msg.transaction_id = tid;
  msg.responding_node_id = own_id;
  return msg;
}

ResponseMessage find_node(const NodeID &own_id,
                          const std::vector<DHTNode> &nodes,
                          const TransactionID &tid) {
  ResponseMessage msg;
  msg.type = MessageType::RESPONSE;
  msg.transaction_id = tid;
  msg.responding_node_id = own_id;
  msg.nodes = nodes;
  return msg;
}

ResponseMessage get_peers_with_nodes(const NodeID &own_id,
                                     const std::vector<DHTNode> &nodes,
                                     const std::string &token,
                                     const TransactionID &tid) {
  ResponseMessage msg;
  msg.type = MessageType::RESPONSE;
  msg.transaction_id = tid;
  msg.responding_node_id = own_id;
  msg.nodes = nodes;
  msg.token = token;
  return msg;
}

ResponseMessage get_peers_with_values(const NodeID &own_id,
                                      const std::vector<DHTNode> &peers,
                                      const std::string &token,
                                      const TransactionID &tid) {
  ResponseMessage msg;
  msg.type = MessageType::RESPONSE;
  msg.transaction_id = tid;
  msg.responding_node_id = own_id;
  msg.peers = peers;
  msg.token = token;
  return msg;
}

ResponseMessage announce_peer(const NodeID &own_id, const TransactionID &tid) {
  ResponseMessage msg;
  msg.type = MessageType::RESPONSE;
  msg.transaction_id = tid;
  msg.responding_node_id = own_id;
  return msg;
}

} // namespace response

// Error builders

namespace error {

ErrorMessage generic_error(const TransactionID &tid,
                           const std::string &message) {
  ErrorMessage msg;
  msg.type = MessageType::ERROR;
  msg.transaction_id = tid;
  msg.error_code = ErrorCode::GENERIC_ERROR;
  msg.error_message = message;
  return msg;
}

ErrorMessage protocol_error(const TransactionID &tid,
                            const std::string &message) {
  ErrorMessage msg;
  msg.type = MessageType::ERROR;
  msg.transaction_id = tid;
  msg.error_code = ErrorCode::PROTOCOL_ERROR;
  msg.error_message = message;
  return msg;
}

ErrorMessage method_unknown(const TransactionID &tid) {
  ErrorMessage msg;
  msg.type = MessageType::ERROR;
  msg.transaction_id = tid;
  msg.error_code = ErrorCode::METHOD_UNKNOWN;
  msg.error_message = "Method Unknown";
  return msg;
}

} // namespace error

} // namespace dht
} // namespace pixelscrape
