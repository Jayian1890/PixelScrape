#include "bencode_parser.hpp"
#include "metadata_exchange.hpp"
#include "sha1.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <json.hpp>
#include <logging.hpp>

namespace pixelscrape {

MetadataExchange::MetadataExchange(const std::array<uint8_t, 20> &info_hash)
    : info_hash_(info_hash), metadata_complete_(false) {}

void MetadataExchange::set_metadata_complete_callback(
    MetadataCompleteCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  complete_callback_ = callback;
}

void MetadataExchange::handle_extension_handshake(
    const std::vector<uint8_t> &payload) {
  try {
    // Parse bencode dictionary
    std::string payload_str(payload.begin(), payload.end());
    BencodeValue parsed = BencodeParser::parse(payload_str);

    auto *dict = std::get_if<std::unique_ptr<BencodeDict>>(&parsed);
    if (!dict || !*dict)
      return;

    // Get metadata_size
    auto size_it = (*dict)->values.find("metadata_size");
    if (size_it != (*dict)->values.end()) {
      if (auto *size_val = std::get_if<BencodeInteger>(&size_it->second)) {
        metadata_size_ = static_cast<size_t>(*size_val);

        // Calculate number of pieces
        size_t num_pieces =
            (*metadata_size_ + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE;
        piece_status_.resize(num_pieces, false);

        pixellib::core::logging::Logger::info(
            "Metadata: Size {} bytes, {} pieces", *metadata_size_, num_pieces);
      }
    }

    // Get extension message IDs
    auto m_it = (*dict)->values.find("m");
    if (m_it != (*dict)->values.end()) {
      auto *m_dict = std::get_if<std::unique_ptr<BencodeDict>>(&m_it->second);
      if (m_dict && *m_dict) {
        auto ut_metadata_it = (*m_dict)->values.find("ut_metadata");
        if (ut_metadata_it != (*m_dict)->values.end()) {
          if (auto *id_val =
                  std::get_if<BencodeInteger>(&ut_metadata_it->second)) {
            peer_ut_metadata_id_ = static_cast<uint8_t>(*id_val);
            pixellib::core::logging::Logger::info(
                "Metadata: Peer ut_metadata ID = {}", *peer_ut_metadata_id_);
          }
        }
      }
    }
  } catch (const std::exception &e) {
    pixellib::core::logging::Logger::error(
        "Metadata: Failed to parse extension handshake: {}", e.what());
  }
}

void MetadataExchange::handle_metadata_message(
    const std::vector<uint8_t> &payload, PeerConnection & /*peer*/) {
  if (payload.empty())
    return;

  try {
    // Find the bencode dictionary end to separate dict from data
    size_t dict_end = 0;
    int depth = 0;
    bool in_dict = false;

    for (size_t i = 0; i < payload.size(); ++i) {
      if (payload[i] == 'd') {
        depth++;
        in_dict = true;
      } else if (payload[i] == 'e' && in_dict) {
        depth--;
        if (depth == 0) {
          dict_end = i + 1;
          break;
        }
      }
    }

    if (dict_end == 0)
      return;

    // Parse the dictionary part
    std::string dict_str(payload.begin(), payload.begin() + dict_end);
    BencodeValue parsed = BencodeParser::parse(dict_str);

    auto *dict = std::get_if<std::unique_ptr<BencodeDict>>(&parsed);
    if (!dict || !*dict)
      return;

    // Get message type
    auto msg_type_it = (*dict)->values.find("msg_type");
    if (msg_type_it == (*dict)->values.end())
      return;

    auto *msg_type_val = std::get_if<BencodeInteger>(&msg_type_it->second);
    if (!msg_type_val)
      return;

    MetadataMessageType msg_type =
        static_cast<MetadataMessageType>(*msg_type_val);

    // Get piece index
    auto piece_it = (*dict)->values.find("piece");
    if (piece_it == (*dict)->values.end())
      return;

    auto *piece_val = std::get_if<BencodeInteger>(&piece_it->second);
    if (!piece_val)
      return;

    size_t piece_index = static_cast<size_t>(*piece_val);

    switch (msg_type) {
    case MetadataMessageType::REQUEST: {
      // Peer is requesting metadata from us
      // We don't have metadata to share (we're downloading)
      if (peer_ut_metadata_id_) {
        auto reject_msg =
            create_metadata_reject(*peer_ut_metadata_id_, piece_index);
        // Send via extended message (would need peer connection support)
      }
      break;
    }

    case MetadataMessageType::DATA: {
      // Peer is sending us metadata
      if (!metadata_size_ || piece_index >= piece_status_.size()) {
        break;
      }

      // Extract data (everything after the dictionary)
      if (dict_end < payload.size()) {
        std::vector<uint8_t> piece_data(payload.begin() + dict_end,
                                        payload.end());

        std::lock_guard<std::mutex> lock(mutex_);

        if (!piece_status_[piece_index]) {
          MetadataPiece piece;
          piece.index = piece_index;
          piece.data = piece_data;
          received_pieces_.push_back(piece);
          piece_status_[piece_index] = true;

          pixellib::core::logging::Logger::info(
              "Metadata: Received piece {}/{}", piece_index + 1,
              piece_status_.size());

          // Check if all pieces received
          bool all_received =
              std::all_of(piece_status_.begin(), piece_status_.end(),
                          [](bool status) { return status; });

          if (all_received) {
            assemble_metadata();
          }
        }
      }
      break;
    }

    case MetadataMessageType::REJECT: {
      pixellib::core::logging::Logger::warning(
          "Metadata: Peer rejected piece {}", piece_index);
      // Could try requesting from another peer
      break;
    }
    }
  } catch (const std::exception &e) {
    pixellib::core::logging::Logger::error(
        "Metadata: Failed to handle message: {}", e.what());
  }
}

void MetadataExchange::request_metadata(PeerConnection & /*peer*/) {
  if (!metadata_size_ || !peer_ut_metadata_id_) {
    pixellib::core::logging::Logger::warning(
        "Metadata: Cannot request - missing size or peer ID");
    return;
  }

  // Request all pieces
  for (size_t i = 0; i < piece_status_.size(); ++i) {
    if (!piece_status_[i]) {
      auto request_msg = create_metadata_request(*peer_ut_metadata_id_, i);
      // Send via extended message (would need peer connection support)
      // For now, this is a placeholder
      pixellib::core::logging::Logger::info("Metadata: Requesting piece {}", i);
    }
  }
}

void MetadataExchange::assemble_metadata() {
  // Sort pieces by index
  std::sort(received_pieces_.begin(), received_pieces_.end(),
            [](const MetadataPiece &a, const MetadataPiece &b) {
              return a.index < b.index;
            });

  // Concatenate all pieces
  complete_metadata_.clear();
  for (const auto &piece : received_pieces_) {
    complete_metadata_.insert(complete_metadata_.end(), piece.data.begin(),
                              piece.data.end());
  }

  // Trim to exact size
  if (metadata_size_ && complete_metadata_.size() > *metadata_size_) {
    complete_metadata_.resize(*metadata_size_);
  }

  // Verify metadata
  if (verify_metadata()) {
    metadata_complete_ = true;
    pixellib::core::logging::Logger::info("Metadata: Complete and verified!");

    if (complete_callback_) {
      complete_callback_(complete_metadata_);
    }
  } else {
    pixellib::core::logging::Logger::error("Metadata: Verification failed!");
    // Reset and try again
    received_pieces_.clear();
    std::fill(piece_status_.begin(), piece_status_.end(), false);
    complete_metadata_.clear();
  }
}

bool MetadataExchange::verify_metadata() {
  if (complete_metadata_.empty())
    return false;

  // Calculate SHA1 hash of metadata
  auto hash = SHA1::hash(complete_metadata_);

  // Compare with info_hash
  return std::equal(hash.begin(), hash.end(), info_hash_.begin());
}

std::vector<uint8_t>
MetadataExchange::create_extension_handshake(bool supports_metadata) {
  // Create bencode dictionary
  std::ostringstream oss;
  oss << "d";

  // Add "m" dictionary with supported extensions
  oss << "1:md";
  if (supports_metadata) {
    oss << "11:ut_metadatai"
        << static_cast<int>(ExtensionMessageID::UT_METADATA) << "e";
  }
  oss << "e";

  // Add client version
  oss << "1:v13:PixelScrape 1.0";

  oss << "e";

  std::string encoded = oss.str();
  return std::vector<uint8_t>(encoded.begin(), encoded.end());
}

std::vector<uint8_t>
MetadataExchange::create_metadata_request(uint8_t /*ut_metadata_id*/,
                                          size_t piece_index) {
  std::ostringstream oss;
  oss << "d";
  oss << "8:msg_typei" << static_cast<int>(MetadataMessageType::REQUEST) << "e";
  oss << "5:piecei" << piece_index << "e";
  oss << "e";

  std::string encoded = oss.str();
  return std::vector<uint8_t>(encoded.begin(), encoded.end());
}

std::vector<uint8_t> MetadataExchange::create_metadata_data(
    uint8_t /*ut_metadata_id*/, size_t piece_index,
    const std::vector<uint8_t> &data, size_t total_size) {
  std::ostringstream oss;
  oss << "d";
  oss << "8:msg_typei" << static_cast<int>(MetadataMessageType::DATA) << "e";
  oss << "5:piecei" << piece_index << "e";
  oss << "10:total_sizei" << total_size << "e";
  oss << "e";

  std::string encoded = oss.str();
  std::vector<uint8_t> result(encoded.begin(), encoded.end());
  result.insert(result.end(), data.begin(), data.end());

  return result;
}

std::vector<uint8_t>
MetadataExchange::create_metadata_reject(uint8_t /*ut_metadata_id*/,
                                         size_t piece_index) {
  std::ostringstream oss;
  oss << "d";
  oss << "8:msg_typei" << static_cast<int>(MetadataMessageType::REJECT) << "e";
  oss << "5:piecei" << piece_index << "e";
  oss << "e";

  std::string encoded = oss.str();
  return std::vector<uint8_t>(encoded.begin(), encoded.end());
}

} // namespace pixelscrape
