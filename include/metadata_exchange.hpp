#pragma once

#include "peer_connection.hpp"
#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace pixelscrape {

// Extension message IDs (negotiated during handshake)
enum class ExtensionMessageID : uint8_t {
  HANDSHAKE = 0,
  UT_METADATA = 1,
  UT_PEX = 2 // For future use
};

// Metadata message types
enum class MetadataMessageType : uint8_t { REQUEST = 0, DATA = 1, REJECT = 2 };

// Metadata piece
struct MetadataPiece {
  size_t index;
  std::vector<uint8_t> data;
};

// Metadata exchange handler
class MetadataExchange {
public:
  static constexpr size_t METADATA_PIECE_SIZE = 16384; // 16 KiB

  using MetadataCompleteCallback =
      std::function<void(const std::vector<uint8_t> &metadata)>;

  MetadataExchange(const std::array<uint8_t, 20> &info_hash);

  // Set callback for when metadata is complete
  void set_metadata_complete_callback(MetadataCompleteCallback callback);

  // Handle extension handshake from peer
  void handle_extension_handshake(const std::vector<uint8_t> &payload);

  // Handle ut_metadata message
  void handle_metadata_message(const std::vector<uint8_t> &payload,
                               PeerConnection &peer);

  // Start requesting metadata from peer
  void request_metadata(PeerConnection &peer);

  // Check if metadata is complete
  bool is_complete() const { return metadata_complete_; }

  // Get completed metadata
  const std::vector<uint8_t> &get_metadata() const {
    return complete_metadata_;
  }

  // Get total metadata size (if known)
  std::optional<size_t> get_metadata_size() const { return metadata_size_; }

  // Create extension handshake message
  static std::vector<uint8_t>
  create_extension_handshake(bool supports_metadata);

  // Create metadata request message
  static std::vector<uint8_t> create_metadata_request(uint8_t ut_metadata_id,
                                                      size_t piece_index);

  // Create metadata data message
  static std::vector<uint8_t>
  create_metadata_data(uint8_t ut_metadata_id, size_t piece_index,
                       const std::vector<uint8_t> &data, size_t total_size);

  // Create metadata reject message
  static std::vector<uint8_t> create_metadata_reject(uint8_t ut_metadata_id,
                                                     size_t piece_index);

private:
  void assemble_metadata();
  bool verify_metadata();

  std::array<uint8_t, 20> info_hash_;
  std::optional<size_t> metadata_size_;
  std::optional<uint8_t> peer_ut_metadata_id_;

  std::vector<MetadataPiece> received_pieces_;
  std::vector<bool> piece_status_; // true if piece received
  std::vector<uint8_t> complete_metadata_;

  bool metadata_complete_;
  MetadataCompleteCallback complete_callback_;

  mutable std::mutex mutex_;
};

} // namespace pixelscrape
