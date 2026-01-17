#pragma once

#include "extension_protocol.hpp"
#include "mse_crypto.hpp"
#include "torrent_metadata.hpp"
#include "tracker_client.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "metadata_exchange.hpp"
#include "piece_manager.hpp"

namespace pixelscrape {

class PieceManager;

enum class PeerMessageType : uint8_t {
  CHOKE = 0,
  UNCHOKE = 1,
  INTERESTED = 2,
  NOT_INTERESTED = 3,
  HAVE = 4,
  BITFIELD = 5,
  REQUEST = 6,
  PIECE = 7,
  CANCEL = 8,
  PORT = 9,
  KEEP_ALIVE = 10,
  EXTENDED = 20 // BEP 0010 extension protocol
};

struct PeerMessage {
  PeerMessageType type;
  std::vector<uint8_t> payload;
};

struct BlockRequest {
  size_t index;
  size_t begin;
  size_t length;
};

struct PieceBlock {
  size_t index;
  size_t begin;
  std::vector<uint8_t> data;
};

class PeerConnection {
public:
  // Constructor for normal leeching/seeding
  PeerConnection(const TorrentMetadata &metadata,
                 const std::array<uint8_t, 20> &info_hash,
                 const std::array<uint8_t, 20> &peer_id,
                 const PeerInfo &peer_info, PieceManager &piece_manager,
                 bool enable_encryption = true, bool enable_extensions = true);

  // Constructor for metadata fetching (magnet links)
  PeerConnection(const std::array<uint8_t, 20> &info_hash,
                 const std::array<uint8_t, 20> &peer_id,
                 const PeerInfo &peer_info, bool enable_encryption = true,
                 bool enable_extensions = true);

  ~PeerConnection();

  // Connection management
  bool connect();
  bool is_connect_complete();
  void start_message_thread();
  void disconnect();
  bool is_connected() const { return connected_; }
  const PeerInfo &get_peer_info() const { return peer_info_; }

  // Message handling
  void send_message(const PeerMessage &message);
  std::optional<PeerMessage> receive_message();

  // State queries
  bool is_choking() const { return peer_choking_; }
  bool is_interested() const { return am_interested_; }
  void set_interested(bool interested);
  const std::vector<bool> &get_bitfield() const { return bitfield_; }
  std::optional<uint16_t> get_dht_port() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dht_port_;
  }

  // Piece management
  void send_have(size_t piece_index);
  void send_request(size_t index, size_t begin, size_t length);
  void send_piece(size_t index, size_t begin, const std::vector<uint8_t> &data);
  void set_have_piece(size_t piece_index, bool have);

  // Callbacks
  using PieceCallback = std::function<void(size_t index, size_t begin,
                                           const std::vector<uint8_t> &data)>;
  void set_piece_callback(PieceCallback cb) { piece_callback_ = cb; }

  // Handshake
  bool perform_handshake();

  // Non-blocking handshake operations
  bool start_handshake();
  bool continue_handshake();
  bool is_handshake_complete() const { return handshake_complete_; }
  bool handshake_successful() const { return handshake_success_; }

  // Extension protocol (BEP 0010)
  void send_extended_message(uint8_t ext_msg_id,
                             const std::vector<uint8_t> &payload);
  void enable_extension_protocol() { supports_extensions_ = true; }
  bool supports_extensions() const { return supports_extensions_; }
  ExtensionProtocol &get_extension_protocol() { return extension_protocol_; }
  const ExtensionProtocol &get_extension_protocol() const {
    return extension_protocol_;
  }

  MetadataExchange &get_metadata_exchange() { return metadata_exchange_; }

  // Encryption (BEP 0008)
  bool is_encrypted() const { return encryption_enabled_; }
  void enable_encryption() { encryption_enabled_ = true; }

private:
  void run();
  void handle_message(const PeerMessage &message);
  void handle_extended_message(const std::vector<uint8_t> &payload);
  bool perform_mse_handshake();
  bool send_encrypted(const std::vector<uint8_t> &data);
  bool recv_encrypted(std::vector<uint8_t> &data, size_t length);

  PieceCallback piece_callback_;
  PeerMessage create_message(PeerMessageType type,
                             const std::vector<uint8_t> &payload = {});
  std::vector<uint8_t> serialize_message(const PeerMessage &message);

  const TorrentMetadata *metadata_;
  std::array<uint8_t, 20> info_hash_;
  std::array<uint8_t, 20> peer_id_;
  PeerInfo peer_info_;
  PieceManager *piece_manager_;

  int socket_fd_;
  std::atomic<bool> connected_;
  std::thread thread_;

  // Peer state
  bool am_choking_;
  bool am_interested_;
  bool peer_choking_;
  bool peer_interested_;
  std::vector<bool> bitfield_;
  std::optional<uint16_t> dht_port_;
  bool supports_extensions_;
  bool encryption_enabled_;
  bool peer_supports_extensions_;

  // Encryption
  std::unique_ptr<MSECrypto> mse_crypto_;
  bool mse_negotiated_;

  // Extension protocol
  ExtensionProtocol extension_protocol_;
  bool extension_handshake_received_;
  MetadataExchange metadata_exchange_;

  // Handshake state for non-blocking operations
  bool handshake_started_;
  bool handshake_complete_;
  bool handshake_success_;
  std::vector<uint8_t> handshake_send_buffer_;
  std::vector<uint8_t> handshake_recv_buffer_;
  size_t handshake_send_pos_;
  size_t handshake_recv_pos_;

  mutable std::mutex mutex_;
};

} // namespace pixelscrape