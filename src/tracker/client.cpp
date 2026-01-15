#include "tracker_client.hpp"
#include <network.hpp>
#include "bencode_parser.hpp"
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace pixelscrape {

TrackerClient::TrackerClient(const TorrentMetadata& metadata) : metadata_(metadata) {}

std::vector<PeerInfo> TrackerClient::get_peers(
    const std::array<uint8_t, 20>& peer_id,
    uint16_t port,
    size_t uploaded,
    size_t downloaded,
    size_t left,
    const std::string& event,
    std::optional<std::string> announce_url
) {
    std::string url = build_tracker_url(peer_id, port, uploaded, downloaded, left, event, announce_url);

    try {
        auto response = pixellib::core::network::Network::http_get(url);
        if (response.empty()) {
            throw std::runtime_error("Empty tracker response");
        }

        // Parse HTTP response to extract body
        size_t body_start = response.find("\r\n\r\n");
        if (body_start == std::string::npos) {
            throw std::runtime_error("Invalid HTTP response format");
        }
        std::string body = response.substr(body_start + 4);

        auto bencode_value = BencodeParser::parse(body);
        if (!std::holds_alternative<std::unique_ptr<BencodeDict>>(bencode_value)) {
            throw std::runtime_error("Invalid tracker response: not a dictionary");
        }

        const auto& response_dict = *std::get<std::unique_ptr<BencodeDict>>(bencode_value);

        // Check for failure reason
        auto failure_it = response_dict.values.find("failure reason");
        if (failure_it != response_dict.values.end() && std::holds_alternative<BencodeString>(failure_it->second)) {
            throw std::runtime_error("Tracker failure: " + std::get<BencodeString>(failure_it->second));
        }

        // Extract peers
        auto peers_it = response_dict.values.find("peers");
        if (peers_it == response_dict.values.end()) {
            throw std::runtime_error("No peers in tracker response");
        }

        if (std::holds_alternative<BencodeString>(peers_it->second)) {
            // Compact peer list
            return parse_compact_peers(std::get<BencodeString>(peers_it->second));
        } else if (std::holds_alternative<std::unique_ptr<BencodeList>>(peers_it->second)) {
            // List of dictionaries
            const auto& peers_list = *std::get<std::unique_ptr<BencodeList>>(peers_it->second);
            std::vector<PeerInfo> peers;
            for (const auto& peer_val : peers_list.items) {
                if (std::holds_alternative<std::unique_ptr<BencodeDict>>(peer_val)) {
                    const auto& peer_dict = *std::get<std::unique_ptr<BencodeDict>>(peer_val);
                    auto ip_it = peer_dict.values.find("ip");
                    auto port_it = peer_dict.values.find("port");
                    
                    if (ip_it != peer_dict.values.end() && port_it != peer_dict.values.end() &&
                        std::holds_alternative<BencodeString>(ip_it->second) &&
                        std::holds_alternative<BencodeInteger>(port_it->second)) {
                        
                        PeerInfo info;
                        std::string ip_str = std::get<BencodeString>(ip_it->second);
                        
                        // Parse IP string (A.B.C.D)
                        int a, b, c, d;
                        if (sscanf(ip_str.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
                            info.ip[0] = static_cast<uint8_t>(a);
                            info.ip[1] = static_cast<uint8_t>(b);
                            info.ip[2] = static_cast<uint8_t>(c);
                            info.ip[3] = static_cast<uint8_t>(d);
                            info.port = static_cast<uint16_t>(std::get<BencodeInteger>(port_it->second));
                            peers.push_back(info);
                        }
                    }
                }
            }
            return peers;
        } else {
            throw std::runtime_error("Unsupported peer format");
        }

    } catch (const std::exception& e) {
        throw std::runtime_error("Tracker request failed: " + std::string(e.what()));
    }
}

std::string TrackerClient::build_tracker_url(
    const std::array<uint8_t, 20>& peer_id,
    uint16_t port,
    size_t uploaded,
    size_t downloaded,
    size_t left,
    const std::string& event,
    std::optional<std::string> announce_url
) const {
    std::stringstream url;
    url << (announce_url ? *announce_url : metadata_.announce);

    // Add query parameters
    url << "?info_hash=" << url_encode(std::string(reinterpret_cast<const char*>(metadata_.info_hash.data()), 20));
    url << "&peer_id=" << url_encode(std::string(reinterpret_cast<const char*>(peer_id.data()), 20));
    url << "&port=" << port;
    url << "&uploaded=" << uploaded;
    url << "&downloaded=" << downloaded;
    url << "&left=" << left;
    url << "&compact=1";

    if (!event.empty()) {
        url << "&event=" << event;
    }

    return url.str();
}

std::vector<PeerInfo> TrackerClient::parse_compact_peers(const std::string& peers_data) const {
    if (peers_data.size() % 6 != 0) {
        throw std::runtime_error("Invalid compact peers data length");
    }

    std::vector<PeerInfo> peers;
    size_t num_peers = peers_data.size() / 6;

    for (size_t i = 0; i < num_peers; ++i) {
        const char* peer_data = peers_data.data() + i * 6;

        PeerInfo peer;
        std::memcpy(peer.ip.data(), peer_data, 4);
        peer.port = (static_cast<uint8_t>(peer_data[4]) << 8) | static_cast<uint8_t>(peer_data[5]);

        peers.push_back(peer);
    }

    return peers;
}

std::string TrackerClient::url_encode(const std::string& data) const {
    std::stringstream encoded;
    encoded << std::hex << std::setfill('0');

    for (char c : data) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else {
            encoded << '%' << std::setw(2) << static_cast<int>(static_cast<uint8_t>(c));
        }
    }

    return encoded.str();
}

std::string PeerInfo::to_string() const {
    std::stringstream ss;
    ss << static_cast<int>(ip[0]) << "."
       << static_cast<int>(ip[1]) << "."
       << static_cast<int>(ip[2]) << "."
       << static_cast<int>(ip[3]) << ":"
       << port;
    return ss.str();
}

} // namespace pixelscrape