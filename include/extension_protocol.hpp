#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <array>
#include "bencode_parser.hpp"

namespace pixelscrape {

/**
 * BitTorrent Extension Protocol (BEP 0010)
 * Enables negotiation of protocol extensions like ut_metadata, ut_pex, etc.
 */
class ExtensionProtocol {
public:
    ExtensionProtocol();

    // Extension message IDs (local mapping)
    enum class ExtensionId : uint8_t {
        HANDSHAKE = 0,
        UT_METADATA = 1,
        UT_PEX = 2,
        LT_DONTHAVE = 7,
        UPLOAD_ONLY = 3
    };

    // Build extended handshake message
    std::vector<uint8_t> build_extended_handshake(
        uint16_t listen_port,
        const std::string& client_name,
        size_t metadata_size = 0) const;

    // Parse extended handshake from peer
    bool parse_extended_handshake(const std::vector<uint8_t>& payload);

    // Extension support queries
    bool supports_ut_metadata() const { return peer_extensions_.count("ut_metadata") > 0; }
    bool supports_ut_pex() const { return peer_extensions_.count("ut_pex") > 0; }
    bool supports_lt_donthave() const { return peer_extensions_.count("lt_donthave") > 0; }
    bool supports_upload_only() const { return peer_extensions_.count("upload_only") > 0; }

    // Get peer's extension message ID for a given extension
    uint8_t get_peer_extension_id(const std::string& extension_name) const;

    // Get our extension message ID for a given extension
    uint8_t get_local_extension_id(ExtensionId ext_id) const;

    // Metadata exchange (BEP 0009)
    std::vector<uint8_t> build_metadata_request(uint8_t ext_msg_id, size_t piece);
    std::vector<uint8_t> build_metadata_data(uint8_t ext_msg_id, size_t piece, 
                                            size_t total_size, const std::vector<uint8_t>& data);
    std::vector<uint8_t> build_metadata_reject(uint8_t ext_msg_id, size_t piece);
    
    // PEX (BEP 0011) - Peer Exchange
    std::vector<uint8_t> build_pex_message(uint8_t ext_msg_id,
                                           const std::vector<std::array<uint8_t, 6>>& added,
                                           const std::vector<std::array<uint8_t, 6>>& dropped);

    // Parse extension messages
    struct MetadataMessage {
        uint8_t msg_type; // 0=request, 1=data, 2=reject
        size_t piece;
        size_t total_size; // only for data messages
        std::vector<uint8_t> data; // only for data messages
    };
    
    struct PexMessage {
        std::vector<std::array<uint8_t, 6>> added;
        std::vector<std::array<uint8_t, 6>> dropped;
    };

    bool parse_metadata_message(const std::vector<uint8_t>& payload, MetadataMessage& msg);
    bool parse_pex_message(const std::vector<uint8_t>& payload, PexMessage& msg);

    // Getters
    const std::string& get_peer_client() const { return peer_client_; }
    uint16_t get_peer_port() const { return peer_port_; }
    size_t get_peer_metadata_size() const { return peer_metadata_size_; }
    bool is_peer_upload_only() const { return peer_upload_only_; }

private:
    // Our supported extensions (name -> local message ID)
    std::map<std::string, uint8_t> local_extensions_;
    
    // Peer's supported extensions (name -> peer's message ID)
    std::map<std::string, uint8_t> peer_extensions_;

    // Peer information from extended handshake
    std::string peer_client_;
    uint16_t peer_port_;
    size_t peer_metadata_size_;
    bool peer_upload_only_;
};

} // namespace pixelscrape
