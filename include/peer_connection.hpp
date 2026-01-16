#pragma once

#include "torrent_metadata.hpp"
#include "tracker_client.hpp"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <array>
#include <functional>

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
    KEEP_ALIVE = 10
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
    PeerConnection(const TorrentMetadata& metadata,
                   const std::array<uint8_t, 20>& info_hash,
                   const std::array<uint8_t, 20>& peer_id,
                   const PeerInfo& peer_info,
                   PieceManager& piece_manager);
    ~PeerConnection();

    // Connection management
    bool connect();
    void disconnect();
    bool is_connected() const { return connected_; }
    const PeerInfo& get_peer_info() const { return peer_info_; }

    // Message handling
    void send_message(const PeerMessage& message);
    std::optional<PeerMessage> receive_message();

    // State queries
    bool is_choking() const { return peer_choking_; }
    bool is_interested() const { return am_interested_; }
    void set_interested(bool interested);
    const std::vector<bool>& get_bitfield() const { return bitfield_; }

    // Piece management
    void send_have(size_t piece_index);
    void send_request(size_t index, size_t begin, size_t length);
    void send_piece(size_t index, size_t begin, const std::vector<uint8_t>& data);
    void set_have_piece(size_t piece_index, bool have);

    // Callbacks
    using PieceCallback = std::function<void(size_t index, size_t begin, const std::vector<uint8_t>& data)>;
    void set_piece_callback(PieceCallback cb) { piece_callback_ = cb; }

    // Handshake
    bool perform_handshake();

private:
    void run();
    void handle_message(const PeerMessage& message);

    PieceCallback piece_callback_;
    PeerMessage create_message(PeerMessageType type, const std::vector<uint8_t>& payload = {});
    std::vector<uint8_t> serialize_message(const PeerMessage& message);

    const TorrentMetadata& metadata_;
    std::array<uint8_t, 20> info_hash_;
    std::array<uint8_t, 20> peer_id_;
    PeerInfo peer_info_;
    PieceManager& piece_manager_;

    int socket_fd_;
    std::atomic<bool> connected_;
    std::thread thread_;

    // Peer state
    bool am_choking_;
    bool am_interested_;
    bool peer_choking_;
    bool peer_interested_;
    std::vector<bool> bitfield_;

    mutable std::mutex mutex_;
};

} // namespace pixelscrape