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
                               PieceManager &piece_manager)
    : metadata_(metadata), info_hash_(info_hash), peer_id_(peer_id),
      peer_info_(peer_info), piece_manager_(piece_manager), socket_fd_(-1),
      connected_(false), am_choking_(true), am_interested_(false),
      peer_choking_(true), peer_interested_(false),
      bitfield_(piece_manager_.get_bitfield()), dht_port_(std::nullopt),
      handshake_buffer_(), handshake_bytes_received_(0) {}

// Incoming-socket constructor (does NOT start the IO thread or assume the
// handshake has completed). Caller should perform handshake validation and
// then call `start()`.
PeerConnection::PeerConnection(int socket_fd, const TorrentMetadata &metadata,
                               const std::array<uint8_t, 20> &info_hash,
                               const std::array<uint8_t, 20> &peer_id,
                               const PeerInfo &peer_info,
                               PieceManager &piece_manager,
                               bool /*owns_socket*/)
    : metadata_(metadata), info_hash_(info_hash), peer_id_(peer_id),
      peer_info_(peer_info), piece_manager_(piece_manager),
      socket_fd_(socket_fd), connected_(false), am_choking_(true),
      am_interested_(false), peer_choking_(true), peer_interested_(false),
      bitfield_(piece_manager_.get_bitfield()), dht_port_(std::nullopt),
      handshake_buffer_(), handshake_bytes_received_(0) {
  // socket is already established; do not start run-loop until handshake
  // completed by caller via `respond_to_handshake` and `start()`.
}

PeerConnection::~PeerConnection() { disconnect(); }

void PeerConnection::disconnect() {
  connected_ = false;
  handshake_buffer_.clear();
  handshake_bytes_received_ = 0;
  if (thread_.joinable()) {
    thread_.join();
  }
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
}

bool PeerConnection::start_connect() {
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
  if (result < 0) {
    if (errno == EINPROGRESS) {
      return true;
    }
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Connected immediately (local?)
  return true;
}

bool PeerConnection::check_connect_result() {
  if (socket_fd_ < 0)
    return false;

  int error = 0;
  socklen_t len = sizeof(error);
  if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0 ||
      error != 0) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }
  return true;
}

bool PeerConnection::send_handshake() {
  // Note: Implicitly assumes socket is writable, caller should check (e.g. via
  // select)
  if (socket_fd_ < 0)
    return false;

  // Create handshake message
  std::vector<uint8_t> handshake;
  handshake.reserve(68);

  // Protocol string length (1 byte)
  handshake.push_back(19);

  // Protocol string
  const char *protocol = "BitTorrent protocol";
  handshake.insert(handshake.end(), protocol, protocol + 19);

  // Reserved bytes (8 bytes)
  handshake.insert(handshake.end(), 8, 0);

  // Info hash (20 bytes)
  handshake.insert(handshake.end(), info_hash_.begin(), info_hash_.end());

  // Peer ID (20 bytes)
  handshake.insert(handshake.end(), peer_id_.begin(), peer_id_.end());

  // Send handshake
  ssize_t sent = send(socket_fd_, handshake.data(), handshake.size(), 0);
  if (sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      sent = 0;
    } else {
      return false;
    }
  }

  if (static_cast<size_t>(sent) < handshake.size()) {
    write_buffer_.insert(write_buffer_.end(), handshake.begin() + sent,
                         handshake.end());
  }

  return true;
}

bool PeerConnection::flush_write_buffer() {
  if (socket_fd_ < 0 || write_buffer_.empty()) {
    return true; // Nothing to write or invalid socket (should we return false
                 // for invalid socket?)
  }

  ssize_t sent =
      send(socket_fd_, write_buffer_.data(), write_buffer_.size(), 0);
  if (sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return false; // Can't write right now
    }
    // Error
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  if (sent > 0) {
    write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + sent);
  }

  return write_buffer_.empty();
}

bool PeerConnection::receive_handshake_nonblocking() {
  if (socket_fd_ < 0)
    return false;

  // Initialize buffer on first call
  if (handshake_buffer_.empty()) {
    handshake_buffer_.resize(68);
    handshake_bytes_received_ = 0;
  }

  // Try to read remaining bytes
  size_t remaining = 68 - handshake_bytes_received_;
  ssize_t received =
      recv(socket_fd_, handshake_buffer_.data() + handshake_bytes_received_,
           remaining, 0);

  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return false; // Not ready yet, try again later
    // Real error - reset and fail
    handshake_buffer_.clear();
    handshake_bytes_received_ = 0;
    return false;
  }

  if (received == 0) {
    // Connection closed
    handshake_buffer_.clear();
    handshake_bytes_received_ = 0;
    return false;
  }

  handshake_bytes_received_ += received;

  // Still waiting for more data
  if (handshake_bytes_received_ < 68)
    return false;

  // We have all 68 bytes - validate
  const char *protocol = "BitTorrent protocol";
  if (handshake_buffer_[0] != 19)
    goto fail;
  if (std::memcmp(handshake_buffer_.data() + 1, protocol, 19) != 0)
    goto fail;
  if (std::memcmp(handshake_buffer_.data() + 28, info_hash_.data(), 20) != 0)
    goto fail;

  // Valid handshake! Reset state
  handshake_buffer_.clear();
  handshake_bytes_received_ = 0;

  // Send unchoke
  send_message(create_message(PeerMessageType::UNCHOKE));
  am_choking_ = false;

  // Send bitfield
  {
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
  }

  return true;

fail:
  handshake_buffer_.clear();
  handshake_bytes_received_ = 0;
  return false;
}

// Keep old connect for backward compat if needed, or replace its gut if no one
// else calls it. The task says "Refactor connect ... or move connection
// establishment". `connection_worker` calls connect(), so we will change
// `connection_worker` to NOT call `connect()` but use these new primitives. We
// can leave `connect()` as a blocking wrapper around these primitives or leave
// as is. Leaving as is seems safer for other potential callers (tests?).
bool PeerConnection::connect() {
  if (!start_connect())
    return false;

  // Blocking wait for write
  fd_set write_fds;
  FD_ZERO(&write_fds);
  FD_SET(socket_fd_, &write_fds);
  struct timeval timeout = {3, 0};
  if (select(socket_fd_ + 1, nullptr, &write_fds, nullptr, &timeout) <= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  if (!check_connect_result())
    return false;

  // Blocking handshake
  if (!send_handshake())
    return false;

  // Flush write buffer if any data remains
  if (!write_buffer_.empty()) {
    while (!write_buffer_.empty()) {
      fd_set write_fds;
      FD_ZERO(&write_fds);
      FD_SET(socket_fd_, &write_fds);
      struct timeval timeout = {3, 0};
      if (select(socket_fd_ + 1, nullptr, &write_fds, nullptr, &timeout) <= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
      }
      if (!flush_write_buffer()) {
        // Either error or still full (but select said writable?)
        // If it was just EAGAIN we loop, but flush_write_buffer returns false
        // on error. Wait, flush_write_buffer returns false on EAGAIN too.
        // We should check if socket is still valid?
        if (socket_fd_ < 0)
          return false;
        // If EAGAIN, we continue loop.
      }
    }
  }

  // Blocking read handshake using old logic logic wrapper?
  // Actually we can reuse `perform_handshake` if we strip the connect check.
  // The original `perform_handshake` assumes blocking socket?
  // We set flag to non-blocking in start_connect.
  // We must restore blocking for `perform_handshake` to work as written
  // originally (recv MSG_WAITALL).

  int flags = fcntl(socket_fd_, F_GETFL, 0);
  fcntl(socket_fd_, F_SETFL, flags & ~O_NONBLOCK);

  if (!perform_handshake()) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  connected_ = true;
  thread_ = std::thread(&PeerConnection::run, this);
  return true;
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
  std::unique_lock<std::mutex> lock(mutex_);

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
    size_t num_bytes = message.payload.size();
    bitfield_.resize(metadata_.piece_hashes.size(), false);
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
      if (!am_choking_ && index < bitfield_.size() && bitfield_[index]) {
        // Read the piece data from piece manager
        auto piece_data = piece_manager_.read_piece(index);
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
        auto cb = piece_callback_;
        lock.unlock();
        cb(index, begin, data);
        lock.lock();
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
  data.reserve(4 + 1 + message.payload.size());

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

  // Reserved bytes (8 bytes)
  handshake.insert(handshake.end(), 8, 0);

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

  return true;
}

// Respond to a handshake we've already received from a remote peer. The
// caller (listener) should parse and validate the remote handshake first;
// this function sends our handshake/bitfield and prepares the peer state.
bool PeerConnection::respond_to_handshake(
    const std::array<uint8_t, 20> &remote_peer_id,
    const std::array<uint8_t, 8> & /*remote_reserved*/) {
  if (socket_fd_ < 0)
    return false;

  // Verify info_hash was validated by listener; double-check here by
  // peeking at the remote info hash if desired (listener already did it).

  // Send our handshake (we are the server-side responder)
  std::vector<uint8_t> handshake;
  handshake.reserve(68);
  handshake.push_back(19);
  const char *protocol = "BitTorrent protocol";
  handshake.insert(handshake.end(), protocol, protocol + 19);
  handshake.insert(handshake.end(), 8, 0);
  handshake.insert(handshake.end(), info_hash_.begin(), info_hash_.end());
  handshake.insert(handshake.end(), peer_id_.begin(), peer_id_.end());

  ssize_t sent = send(socket_fd_, handshake.data(), handshake.size(), 0);
  if (sent != static_cast<ssize_t>(handshake.size())) {
    return false;
  }

  // Record remote peer id (we received it on the listener side)
  // Note: PeerConnection currently stores our own peer_id_ (local). If we
  // want to record the remote peer id for logging/extension purposes, we
  // can keep it in a transient variable — for now the listener has it.

  // Become willing to upload
  send_message(create_message(PeerMessageType::UNCHOKE));
  am_choking_ = false;

  // Send our bitfield immediately if we have pieces
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

  return true;
}

void PeerConnection::start() {
  if (connected_)
    return;

  // Restore blocking mode for the I/O thread
  // The socket was set to non-blocking during connection establishment,
  // but the run() thread expects blocking I/O (uses MSG_WAITALL)
  if (socket_fd_ >= 0) {
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(socket_fd_, F_SETFL, flags & ~O_NONBLOCK);
    }
  }

  connected_ = true;
  thread_ = std::thread(&PeerConnection::run, this);
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
    uint8_t extended_msg_id, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> extended_payload;
  extended_payload.reserve(1 + payload.size());
  extended_payload.push_back(extended_msg_id);
  extended_payload.insert(extended_payload.end(), payload.begin(),
                          payload.end());
  send_message(create_message(PeerMessageType::EXTENDED, extended_payload));
}

} // namespace pixelscrape