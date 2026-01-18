#include "metadata_exchange.hpp"
#include "peer_connection.hpp"
#include <cstring>
#include <extension_protocol.hpp>
#include <logging.hpp>
#include <sha1.hpp>

namespace pixelscrape {

MetadataExchange::MetadataExchange(const std::array<uint8_t, 20> &info_hash)
    : info_hash_(info_hash), metadata_complete_(false) {}

void MetadataExchange::set_metadata_complete_callback(
    MetadataCompleteCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  complete_callback_ = std::move(callback);
}

void MetadataExchange::handle_extension_handshake(
    const std::vector<uint8_t> &payload) {
  ExtensionProtocol ext;
  if (!ext.parse_extended_handshake(payload)) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (ext.supports_ut_metadata()) {
    peer_ut_metadata_id_ = ext.get_peer_extension_id("ut_metadata");
  } else {
    peer_ut_metadata_id_ = std::nullopt;
  }

  if (ext.get_peer_metadata_size() > 0) {
    if (!metadata_size_) {
      metadata_size_ = ext.get_peer_metadata_size();

      // Calculate number of pieces
      size_t num_pieces =
          (*metadata_size_ + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE;
      piece_status_.resize(num_pieces, false);
      received_pieces_.resize(num_pieces);

      pixellib::core::logging::Logger::info(
          "Metadata size: {} bytes ({} pieces)", *metadata_size_, num_pieces);
    } else if (*metadata_size_ != ext.get_peer_metadata_size()) {
      pixellib::core::logging::Logger::warning(
          "Peer reports different metadata size: {} (expected {})",
          ext.get_peer_metadata_size(), *metadata_size_);
    }
  }
}

void MetadataExchange::handle_metadata_message(
    const std::vector<uint8_t> &payload, PeerConnection &peer) {
  ExtensionProtocol::MetadataMessage msg;
  ExtensionProtocol ext;
  if (!ext.parse_metadata_message(payload, msg)) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  switch (msg.msg_type) {
  case 0: // Request
    // We only support being a client for now (downloading metadata)
    // In future, we would serve metadata here if we have it
    // For now, send reject
    {
      if (peer_ut_metadata_id_) {
        auto reject = create_metadata_reject(*peer_ut_metadata_id_, msg.piece);
        peer.send_extended_message(*peer_ut_metadata_id_, reject);
      }
    }
    break;

  case 1: // Data
    if (!metadata_size_ || msg.piece >= piece_status_.size()) {
      return;
    }

    // Store piece
    if (!piece_status_[msg.piece]) {
      MetadataPiece piece;
      piece.index = msg.piece;
      piece.data = msg.data;
      received_pieces_[msg.piece] = std::move(piece);
      piece_status_[msg.piece] = true;

      pixellib::core::logging::Logger::debug("Received metadata piece {}/{}",
                                             msg.piece, piece_status_.size());

      assemble_metadata();
    }
    break;

  case 2: // Reject
    pixellib::core::logging::Logger::warning(
        "Peer rejected metadata request for piece {}", msg.piece);
    break;
  }
}

void MetadataExchange::request_metadata(PeerConnection &peer) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!peer_ut_metadata_id_ || !metadata_size_ || metadata_complete_) {
    return;
  }

  // Find missing pieces
  for (size_t i = 0; i < piece_status_.size(); ++i) {
    if (!piece_status_[i]) {
      // Send request
      auto req = create_metadata_request(*peer_ut_metadata_id_, i);
      peer.send_extended_message(*peer_ut_metadata_id_, req);
      // Simple strategy: request one by one (or rely on upper layer loop)
      // For now, let's request them all (simple, maybe aggressive)
    }
  }
}

void MetadataExchange::assemble_metadata() {
  // Check if we have all pieces
  for (bool received : piece_status_) {
    if (!received)
      return;
  }

  // Assemble
  std::vector<uint8_t> buffer;
  for (const auto &piece : received_pieces_) {
    buffer.insert(buffer.end(), piece.data.begin(), piece.data.end());
  }

  if (buffer.size() != *metadata_size_) {
    pixellib::core::logging::Logger::error(
        "Metadata assembly size mismatch: {} != {}", buffer.size(),
        *metadata_size_);
    return;
  }

  complete_metadata_ = std::move(buffer);

  if (verify_metadata()) {
    metadata_complete_ = true;
    pixellib::core::logging::Logger::info(
        "Metadata download complete and verified!");
    if (complete_callback_) {
      complete_callback_(complete_metadata_);
    }
  } else {
    pixellib::core::logging::Logger::error(
        "Metadata verification failed (hash mismatch)");
    // Reset? or keep pieces?
    // For now, keep state to allow retry/debug
  }
}

bool MetadataExchange::verify_metadata() {
  if (complete_metadata_.empty())
    return false;

  SHA1 sha1;
  sha1.update(complete_metadata_);
  std::array<uint8_t, 20> hash = sha1.finalize();

  return hash == info_hash_;
}

// Static helpers delegated from ExtensionProtocol for convenience/wrapping
std::vector<uint8_t>
MetadataExchange::create_extension_handshake(bool supports_metadata) {
  ExtensionProtocol ext;
  return {};
}

std::vector<uint8_t>
MetadataExchange::create_metadata_request(uint8_t ut_metadata_id,
                                          size_t piece_index) {
  ExtensionProtocol ext;
  return ext.build_metadata_request(ut_metadata_id, piece_index);
}

std::vector<uint8_t> MetadataExchange::create_metadata_data(
    uint8_t ut_metadata_id, size_t piece_index,
    const std::vector<uint8_t> &data, size_t total_size) {
  ExtensionProtocol ext;
  return ext.build_metadata_data(ut_metadata_id, piece_index, total_size, data);
}

std::vector<uint8_t>
MetadataExchange::create_metadata_reject(uint8_t ut_metadata_id,
                                         size_t piece_index) {
  ExtensionProtocol ext;
  return ext.build_metadata_reject(ut_metadata_id, piece_index);
}

} // namespace pixelscrape
