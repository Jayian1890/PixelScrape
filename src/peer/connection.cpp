#include "peer_connection.hpp"
#include "piece_manager.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace pixelscrape {

PeerConnection::PeerConnection(const TorrentMetadata &metadata,
                               const std::array<uint8_t, 20> &info_hash,
                               const std::array<uint8_t, 20> &peer_id,
                               const PeerInfo &peer_info,
                               PieceManager &piece_manager,
                               bool enable_encryption, bool enable_extensions)
    : metadata_(&metadata), info_hash_(info_hash), peer_id_(peer_id),
      peer_info_(peer_info), piece_manager_(&piece_manager), socket_fd_(-1),
      connected_(false), am_choking_(true), am_interested_(false),
      peer_choking_(true), peer_interested_(false),
      bitfield_(piece_manager.get_bitfield()), dht_port_(std::nullopt),
      supports_extensions_(enable_extensions),
      encryption_enabled_(enable_encryption), peer_supports_extensions_(false),
      mse_crypto_(nullptr), mse_negotiated_(false),
      extension_handshake_received_(false), metadata_exchange_(info_hash) {
  if (encryption_enabled_) {
    mse_crypto_ = std::make_unique<MSECrypto>();
  }
}

PeerConnection::PeerConnection(const std::array<uint8_t, 20> &info_hash,
                               const std::array<uint8_t, 20> &peer_id,
                               const PeerInfo &peer_info,
                               bool enable_encryption, bool enable_extensions)
    : metadata_(nullptr), info_hash_(info_hash), peer_id_(peer_id),
      peer_info_(peer_info), piece_manager_(nullptr), socket_fd_(-1),
      connected_(false), am_choking_(true), am_interested_(false),
      peer_choking_(true), peer_interested_(false), dht_port_(std::nullopt),
      supports_extensions_(enable_extensions),
      encryption_enabled_(enable_encryption), peer_supports_extensions_(false),
      mse_crypto_(nullptr), mse_negotiated_(false),
      extension_handshake_received_(false), metadata_exchange_(info_hash) {
  if (encryption_enabled_) {
    mse_crypto_ = std::make_unique<MSECrypto>();
  }
}

PeerConnection::~PeerConnection() { disconnect(); }

bool PeerConnection::connect() {
  if (connected_)
    return true;

  socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    return false;
  }

  // Set non-blocking
  int flags = fcntl(socket_fd_, F_GETFL, 0);
  fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(peer_info_.port);
  std::memcpy(&addr.sin_addr, peer_info_.ip.data(), 4);

  int result =
      ::connect(socket_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  if (result < 0 && errno != EINPROGRESS) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Wait for connection with timeout
  fd_set write_fds;
  FD_ZERO(&write_fds);
  FD_SET(socket_fd_, &write_fds);

  struct timeval timeout = {3, 0}; // 3 seconds
  result = select(socket_fd_ + 1, nullptr, &write_fds, nullptr, &timeout);

  if (result <= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Check if connection was successful
  int error;
  socklen_t len = sizeof(error);
  getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &len);
  if (error != 0) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Set back to blocking
  fcntl(socket_fd_, F_SETFL, flags);

  connected_ = true;
  thread_ = std::thread(&PeerConnection::run, this);
  return true;
}

void PeerConnection::disconnect() {
  connected_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
}

void PeerConnection::run() {
  while (connected_) {
    auto message = receive_message();
    if (message) {
      handle_message(*message);
    } else {
      // Connection lost or timeout
      connected_ = false;
      break;
    }
  }
}

void PeerConnection::send_message(const PeerMessage &message) {
  if (!connected_)
    return;

  auto data = serialize_message(message);
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t result =
        send(socket_fd_, data.data() + sent, data.size() - sent, 0);
    if (result <= 0) {
      connected_ = false;
      return;
    }
    sent += result;
  }
}

std::optional<PeerMessage> PeerConnection::receive_message() {
  if (!connected_)
    return std::nullopt;

  // Read message length (4 bytes, big-endian)
  uint32_t length;
  ssize_t result = recv(socket_fd_, &length, 4, MSG_WAITALL);
  if (result != 4) {
    return std::nullopt;
  }

  length = ntohl(length);
  if (length == 0) {
    return PeerMessage{PeerMessageType::KEEP_ALIVE, {}};
  }

  if (length > 1024 * 1024) { // 1MB limit
    connected_ = false;
    return std::nullopt;
  }

  std::vector<uint8_t> payload(length);
  result = recv(socket_fd_, payload.data(), length, MSG_WAITALL);
  if (result != static_cast<ssize_t>(length)) {
    return std::nullopt;
  }

  if (payload.empty()) {
    return std::nullopt; // Invalid message
  }

  PeerMessageType type = static_cast<PeerMessageType>(payload[0]);
  payload.erase(payload.begin()); // Remove type byte

  return PeerMessage{type, payload};
}

void PeerConnection::set_interested(bool interested) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (am_interested_ == interested)
    return;

  am_interested_ = interested;
  if (am_interested_) {
    send_message(create_message(PeerMessageType::INTERESTED));
  } else {
    send_message(create_message(PeerMessageType::NOT_INTERESTED));
  }
}

void PeerConnection::handle_message(const PeerMessage &message) {
  std::lock_guard<std::mutex> lock(mutex_);

  switch (message.type) {
  case PeerMessageType::CHOKE:
    peer_choking_ = true;
    break;
  case PeerMessageType::UNCHOKE:
    peer_choking_ = false;
    break;
  case PeerMessageType::INTERESTED:
    peer_interested_ = true;
    break;
  case PeerMessageType::NOT_INTERESTED:
    peer_interested_ = false;
    break;
  case PeerMessageType::HAVE: {
    if (message.payload.size() >= 4) {
      uint32_t piece_index =
          ntohl(*reinterpret_cast<const uint32_t *>(message.payload.data()));
      if (piece_index < bitfield_.size()) {
        bitfield_[piece_index] = true;
      }
    }
    break;
  }
  case PeerMessageType::BITFIELD: {
    if (!metadata_)
      break; // Cannot handle bitfield without metadata (to know size)

    size_t num_bytes = message.payload.size();
    bitfield_.resize(metadata_->piece_hashes.size(), false);
    for (size_t i = 0; i < num_bytes && i * 8 < bitfield_.size(); ++i) {
      uint8_t byte = message.payload[i];
      for (size_t j = 0; j < 8 && i * 8 + j < bitfield_.size(); ++j) {
        bitfield_[i * 8 + j] = (byte & (1 << (7 - j))) != 0;
      }
    }
    break;
  }
  case PeerMessageType::REQUEST:
    // Handle piece requests (for seeding)
    if (message.payload.size() >= 12) {
      uint32_t index =
          ntohl(*reinterpret_cast<const uint32_t *>(message.payload.data()));
      uint32_t begin = ntohl(
          *reinterpret_cast<const uint32_t *>(message.payload.data() + 4));
      uint32_t length = ntohl(
          *reinterpret_cast<const uint32_t *>(message.payload.data() + 8));

      // Check if we have the piece and are not choking
      if (piece_manager_ && !am_choking_ && index < bitfield_.size() &&
          bitfield_[index]) {
        // Read the piece data from piece manager
        auto piece_data = piece_manager_->read_piece(index);
        if (!piece_data.empty() && begin + length <= piece_data.size()) {
          std::vector<uint8_t> block_data(piece_data.begin() + begin,
                                          piece_data.begin() + begin + length);
          send_piece(index, begin, block_data);
        }
      }
    }
    break;
  case PeerMessageType::PIECE:
    // Handle received piece blocks
    if (message.payload.size() >= 8) {
      uint32_t index =
          ntohl(*reinterpret_cast<const uint32_t *>(message.payload.data()));
      uint32_t begin = ntohl(
          *reinterpret_cast<const uint32_t *>(message.payload.data() + 4));
      std::vector<uint8_t> data(message.payload.begin() + 8,
                                message.payload.end());

      if (piece_callback_) {
        piece_callback_(index, begin, data);
      }
    }
    break;
  case PeerMessageType::CANCEL:
    // Handle cancel requests - in a full implementation, remove from request
    // queue
    break;
  case PeerMessageType::PORT:
    if (message.payload.size() >= 2) {
      uint16_t port_be;
      std::memcpy(&port_be, message.payload.data(), sizeof(port_be));
      dht_port_ = ntohs(port_be);
    }
    break;
  case PeerMessageType::EXTENDED:
    // Handle extended messages (BEP 0010)
    if (!message.payload.empty()) {
      handle_extended_message(message.payload);
    }
    break;
  case PeerMessageType::KEEP_ALIVE:
    break;
  }
}

PeerMessage
PeerConnection::create_message(PeerMessageType type,
                               const std::vector<uint8_t> &payload) {
  return {type, payload};
}

std::vector<uint8_t>
PeerConnection::serialize_message(const PeerMessage &message) {
  std::vector<uint8_t> data;

  // Add payload length (4 bytes, big-endian)
  uint32_t length = 1 + message.payload.size(); // +1 for message type
  uint32_t length_be = htonl(length);
  data.insert(data.end(), reinterpret_cast<uint8_t *>(&length_be),
              reinterpret_cast<uint8_t *>(&length_be) + 4);

  // Add message type
  data.push_back(static_cast<uint8_t>(message.type));

  // Add payload
  data.insert(data.end(), message.payload.begin(), message.payload.end());

  return data;
}

bool PeerConnection::perform_handshake() {
  if (!connected_)
    return false;

  // Create handshake message
  std::vector<uint8_t> handshake;
  handshake.reserve(68);

  // Protocol string length (1 byte)
  handshake.push_back(19);

  // Protocol string
  const char *protocol = "BitTorrent protocol";
  handshake.insert(handshake.end(), protocol, protocol + 19);

  // Reserved bytes (8 bytes) - set extension bit if supported
  std::array<uint8_t, 8> reserved = {0};
  if (supports_extensions_) {
    reserved[5] |= 0x10; // Extension protocol bit (BEP 0010)
  }
  handshake.insert(handshake.end(), reserved.begin(), reserved.end());

  // Info hash (20 bytes)
  handshake.insert(handshake.end(), info_hash_.begin(), info_hash_.end());

  // Peer ID (20 bytes)
  handshake.insert(handshake.end(), peer_id_.begin(), peer_id_.end());

  // Send handshake
  ssize_t sent = send(socket_fd_, handshake.data(), handshake.size(), 0);
  if (sent != static_cast<ssize_t>(handshake.size())) {
    return false;
  }

  // Receive handshake response
  std::vector<uint8_t> response(68);
  ssize_t received =
      recv(socket_fd_, response.data(), response.size(), MSG_WAITALL);
  if (received != 68) {
    return false;
  }

  // Verify response
  if (response[0] != 19)
    return false;
  if (std::memcmp(response.data() + 1, protocol, 19) != 0)
    return false;
  if (std::memcmp(response.data() + 28, info_hash_.data(), 20) != 0)
    return false;

  // Check if peer supports extensions (bit 20 in reserved bytes)
  peer_supports_extensions_ = (response[25] & 0x10) != 0;

  // Send unchoke to allow downloading from us
  send_message(create_message(PeerMessageType::UNCHOKE));
  am_choking_ = false;

  // Send bitfield if we have any pieces
  std::vector<uint8_t> bitfield_payload;
  bool has_pieces = false;
  int bit_count = 0;
  uint8_t byte = 0;
  for (bool has : bitfield_) {
    if (has)
      has_pieces = true;
    if (has)
      byte |= (1 << (7 - bit_count));
    bit_count++;
    if (bit_count == 8) {
      bitfield_payload.push_back(byte);
      byte = 0;
      bit_count = 0;
    }
  }
  if (bit_count > 0) {
    bitfield_payload.push_back(byte);
  }
  if (has_pieces) {
    send_message(create_message(PeerMessageType::BITFIELD, bitfield_payload));
  }

  // Send extended handshake if both support extensions
  if (supports_extensions_ && peer_supports_extensions_) {
    size_t meta_size = 0;
    if (metadata_) {
      // We have metadata
      // In a real implementation we would calculate the size.
      // For now, if we have metadata, we advertise a non-zero size if we were
      // to support serving
    }

    auto ext_handshake = extension_protocol_.build_extended_handshake(
        6881, // Default port
        "PixelScrape/1.0.0", meta_size);
    send_extended_message(0, ext_handshake); // 0 = extended handshake
  }

  return true;
}

void PeerConnection::send_have(size_t piece_index) {
  uint32_t index_be = htonl(piece_index);
  std::vector<uint8_t> payload(reinterpret_cast<uint8_t *>(&index_be),
                               reinterpret_cast<uint8_t *>(&index_be) + 4);
  send_message(create_message(PeerMessageType::HAVE, payload));
}

void PeerConnection::send_request(size_t index, size_t begin, size_t length) {
  std::vector<uint8_t> payload(12);
  *reinterpret_cast<uint32_t *>(&payload[0]) = htonl(index);
  *reinterpret_cast<uint32_t *>(&payload[4]) = htonl(begin);
  *reinterpret_cast<uint32_t *>(&payload[8]) = htonl(length);
  send_message(create_message(PeerMessageType::REQUEST, payload));
}

void PeerConnection::send_piece(size_t index, size_t begin,
                                const std::vector<uint8_t> &data) {
  std::vector<uint8_t> payload(8 + data.size());
  *reinterpret_cast<uint32_t *>(&payload[0]) = htonl(index);
  *reinterpret_cast<uint32_t *>(&payload[4]) = htonl(begin);
  std::memcpy(&payload[8], data.data(), data.size());
  send_message(create_message(PeerMessageType::PIECE, payload));
}

void PeerConnection::set_have_piece(size_t piece_index, bool have) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (piece_index < bitfield_.size()) {
    bitfield_[piece_index] = have;
    if (have) {
      send_have(piece_index);
    }
  }
}

void PeerConnection::send_extended_message(
    uint8_t ext_msg_id, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> full_payload;
  full_payload.push_back(ext_msg_id);
  full_payload.insert(full_payload.end(), payload.begin(), payload.end());
  send_message(create_message(PeerMessageType::EXTENDED, full_payload));
}

void PeerConnection::handle_extended_message(
    const std::vector<uint8_t> &payload) {
  if (payload.empty()) {
    return;
  }

  uint8_t ext_msg_id = payload[0];
  std::vector<uint8_t> ext_payload(payload.begin() + 1, payload.end());

  // Extended handshake (ID 0)
  if (ext_msg_id == 0) {
    if (extension_protocol_.parse_extended_handshake(ext_payload)) {
      extension_handshake_received_ = true;

      // Pass handshake to metadata exchange
      metadata_exchange_.handle_extension_handshake(ext_payload);

      // If we need metadata, start requesting it
      if (!metadata_ && !metadata_exchange_.is_complete()) {
        metadata_exchange_.request_metadata(*this);
      }
    }
    return;
  }

  // Handle ut_metadata messages
  if (extension_protocol_.supports_ut_metadata()) {
    uint8_t ut_metadata_id =
        extension_protocol_.get_peer_extension_id("ut_metadata");
    if (ext_msg_id == ut_metadata_id) {
      metadata_exchange_.handle_metadata_message(ext_payload, *this);

      // If we don't have metadata yet, try to request more if possible
      if (!metadata_ && !metadata_exchange_.is_complete()) {
        metadata_exchange_.request_metadata(*this);
      }
      return;
    }
  }

  // Handle ut_pex messages
  if (extension_protocol_.supports_ut_pex()) {
    uint8_t ut_pex_id = extension_protocol_.get_peer_extension_id("ut_pex");
    if (ext_msg_id == ut_pex_id) {
      ExtensionProtocol::PexMessage pex_msg;
      if (extension_protocol_.parse_pex_message(ext_payload, pex_msg)) {
        // Handle PEX - add new peers to discovered list
        // TODO: Pass added peers to TorrentManager for connection attempts
      }
      return;
    }
  }
}

bool PeerConnection::perform_mse_handshake() {
  // MSE/PE handshake implementation
  // This is a complex protocol - simplified version here

  if (!mse_crypto_ || !encryption_enabled_) {
    return false;
  }

  try {
    // Generate DH keypair
    mse_crypto_->generate_dh_keypair();
    auto public_key = mse_crypto_->get_public_key();

    // Send our public key
    if (send(socket_fd_, public_key.data(), public_key.size(), 0) !=
        static_cast<ssize_t>(public_key.size())) {
      return false;
    }

    // Receive peer's public key (96 bytes)
    std::vector<uint8_t> peer_public_key(96);
    if (recv(socket_fd_, peer_public_key.data(), 96, MSG_WAITALL) != 96) {
      return false;
    }

    // Compute shared secret
    if (!mse_crypto_->compute_shared_secret(peer_public_key)) {
      return false;
    }

    // Continue with crypto_provide/select exchange
    auto crypto_provide = mse_crypto_->compute_crypto_provide(
        info_hash_, mse_crypto_->get_shared_secret());

    // For now, we'll use plaintext mode (crypto_select = 0x01)
    // Full implementation would negotiate RC4 encryption

    mse_negotiated_ = true;
    return true;

  } catch (...) {
    return false;
  }
}

bool PeerConnection::send_encrypted(const std::vector<uint8_t> &data) {
  if (!mse_crypto_ || !mse_negotiated_) {
    // Send plaintext
    return send(socket_fd_, data.data(), data.size(), 0) ==
           static_cast<ssize_t>(data.size());
  }

  // Encrypt and send
  std::vector<uint8_t> encrypted_data = data;
  mse_crypto_->encrypt(encrypted_data);
  return send(socket_fd_, encrypted_data.data(), encrypted_data.size(), 0) ==
         static_cast<ssize_t>(encrypted_data.size());
}

bool PeerConnection::recv_encrypted(std::vector<uint8_t> &data, size_t length) {
  data.resize(length);

  if (recv(socket_fd_, data.data(), length, MSG_WAITALL) !=
      static_cast<ssize_t>(length)) {
    return false;
  }

  if (mse_crypto_ && mse_negotiated_) {
    // Decrypt
    mse_crypto_->decrypt(data);
  }

  return true;
}

} // namespace pixelscrape