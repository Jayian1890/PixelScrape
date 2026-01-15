#pragma once

#include "torrent_metadata.hpp"
#include <vector>
#include <string>
#include <array>
#include <optional>

namespace pixelscrape {

struct PeerInfo {
    std::array<uint8_t, 4> ip;
    uint16_t port;

    std::string to_string() const;
};

class TrackerClient {
public:
    TrackerClient(const TorrentMetadata& metadata);

    // Get peers from tracker
    std::vector<PeerInfo> get_peers(
        const std::array<uint8_t, 20>& peer_id,
        uint16_t port,
        size_t uploaded,
        size_t downloaded,
        size_t left,
        const std::string& event = ""
    );

private:
    std::string build_tracker_url(
        const std::array<uint8_t, 20>& peer_id,
        uint16_t port,
        size_t uploaded,
        size_t downloaded,
        size_t left,
        const std::string& event
    ) const;

    std::vector<PeerInfo> parse_compact_peers(const std::string& peers_data) const;
    std::string url_encode(const std::string& data) const;

    const TorrentMetadata& metadata_;
};

} // namespace pixelscrape