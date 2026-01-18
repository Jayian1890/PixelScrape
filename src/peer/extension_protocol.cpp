#include "extension_protocol.hpp"
#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <array>

namespace pixelscrape {

ExtensionProtocol::ExtensionProtocol() 
    : peer_port_(0), peer_metadata_size_(0), peer_upload_only_(false) {
    // Register our supported extensions with local message IDs
    local_extensions_["ut_metadata"] = static_cast<uint8_t>(ExtensionId::UT_METADATA);
    local_extensions_["ut_pex"] = static_cast<uint8_t>(ExtensionId::UT_PEX);
    local_extensions_["lt_donthave"] = static_cast<uint8_t>(ExtensionId::LT_DONTHAVE);
    local_extensions_["upload_only"] = static_cast<uint8_t>(ExtensionId::UPLOAD_ONLY);
}

std::vector<uint8_t> ExtensionProtocol::build_extended_handshake(
    uint16_t listen_port,
    const std::string& client_name,
    size_t metadata_size) const {
    
    auto dict = std::make_unique<BencodeDict>();
    
    // Add supported extensions
    auto m = std::make_unique<BencodeDict>();
    for (const auto& [name, id] : local_extensions_) {
        m->values[name] = static_cast<BencodeInteger>(id);
    }
    dict->values["m"] = std::move(m);
    
    // Add client name/version
    dict->values["v"] = client_name;
    
    // Add listening port
    dict->values["p"] = static_cast<BencodeInteger>(listen_port);
    
    // Add metadata size if available
    if (metadata_size > 0) {
        dict->values["metadata_size"] = static_cast<BencodeInteger>(metadata_size);
    }
    
    // Add other optional fields
    dict->values["reqq"] = static_cast<BencodeInteger>(250); // Request queue depth
    
    // Convert to bencoded format
    std::string bencoded = BencodeParser::encode(std::move(dict));
    
    // Convert string to bytes
    std::vector<uint8_t> payload(bencoded.begin(), bencoded.end());
    
    return payload;
}

bool ExtensionProtocol::parse_extended_handshake(const std::vector<uint8_t>& payload) {
    try {
        // Parse bencoded data
        std::string payload_str(payload.begin(), payload.end());
        auto val = BencodeParser::parse(payload_str);
        
        if (!std::holds_alternative<std::unique_ptr<BencodeDict>>(val)) {
            return false;
        }
        
        const auto& dict = *std::get<std::unique_ptr<BencodeDict>>(val);

        // Parse 'm' dictionary (extensions)
        if (dict.values.count("m") && std::holds_alternative<std::unique_ptr<BencodeDict>>(dict.values.at("m"))) {
            const auto& m = *std::get<std::unique_ptr<BencodeDict>>(dict.values.at("m"));
            peer_extensions_.clear();
            for (const auto& [key, value] : m.values) {
                if (std::holds_alternative<BencodeInteger>(value)) {
                    peer_extensions_[key] = static_cast<uint8_t>(std::get<BencodeInteger>(value));
                }
            }
        }
        
        // Parse client name/version
        if (dict.values.count("v") && std::holds_alternative<BencodeString>(dict.values.at("v"))) {
            peer_client_ = std::get<BencodeString>(dict.values.at("v"));
        }
        
        // Parse listening port
        if (dict.values.count("p") && std::holds_alternative<BencodeInteger>(dict.values.at("p"))) {
            peer_port_ = static_cast<uint16_t>(std::get<BencodeInteger>(dict.values.at("p")));
        }
        
        // Parse metadata size
        if (dict.values.count("metadata_size") && std::holds_alternative<BencodeInteger>(dict.values.at("metadata_size"))) {
            peer_metadata_size_ = static_cast<size_t>(std::get<BencodeInteger>(dict.values.at("metadata_size")));
        }
        
        // Parse upload_only flag
        if (dict.values.count("upload_only") && std::holds_alternative<BencodeInteger>(dict.values.at("upload_only"))) {
            peer_upload_only_ = std::get<BencodeInteger>(dict.values.at("upload_only")) != 0;
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

uint8_t ExtensionProtocol::get_peer_extension_id(const std::string& extension_name) const {
    auto it = peer_extensions_.find(extension_name);
    if (it != peer_extensions_.end()) {
        return it->second;
    }
    return 0; // 0 means not supported
}

uint8_t ExtensionProtocol::get_local_extension_id(ExtensionId ext_id) const {
    return static_cast<uint8_t>(ext_id);
}

std::vector<uint8_t> ExtensionProtocol::build_metadata_request(uint8_t ext_msg_id, size_t piece) {
    auto dict = std::make_unique<BencodeDict>();
    dict->values["msg_type"] = static_cast<BencodeInteger>(0); // request
    dict->values["piece"] = static_cast<BencodeInteger>(piece);
    
    std::string bencoded = BencodeParser::encode(std::move(dict));
    
    std::vector<uint8_t> payload;
    payload.push_back(ext_msg_id); // Extended message ID
    payload.insert(payload.end(), bencoded.begin(), bencoded.end());
    
    return payload;
}

std::vector<uint8_t> ExtensionProtocol::build_metadata_data(
    uint8_t ext_msg_id, size_t piece, size_t total_size, const std::vector<uint8_t>& data) {
    
    auto dict = std::make_unique<BencodeDict>();
    dict->values["msg_type"] = static_cast<BencodeInteger>(1); // data
    dict->values["piece"] = static_cast<BencodeInteger>(piece);
    dict->values["total_size"] = static_cast<BencodeInteger>(total_size);
    
    std::string bencoded = BencodeParser::encode(std::move(dict));
    
    std::vector<uint8_t> payload;
    payload.push_back(ext_msg_id);
    payload.insert(payload.end(), bencoded.begin(), bencoded.end());
    payload.insert(payload.end(), data.begin(), data.end());
    
    return payload;
}

std::vector<uint8_t> ExtensionProtocol::build_metadata_reject(uint8_t ext_msg_id, size_t piece) {
    auto dict = std::make_unique<BencodeDict>();
    dict->values["msg_type"] = static_cast<BencodeInteger>(2); // reject
    dict->values["piece"] = static_cast<BencodeInteger>(piece);
    
    std::string bencoded = BencodeParser::encode(std::move(dict));
    
    std::vector<uint8_t> payload;
    payload.push_back(ext_msg_id);
    payload.insert(payload.end(), bencoded.begin(), bencoded.end());
    
    return payload;
}

std::vector<uint8_t> ExtensionProtocol::build_pex_message(
    uint8_t ext_msg_id,
    const std::vector<std::array<uint8_t, 6>>& added,
    const std::vector<std::array<uint8_t, 6>>& dropped) {
    
    auto dict = std::make_unique<BencodeDict>();
    
    // Convert added peers to binary string (IP:port pairs, 6 bytes each)
    std::string added_str;
    for (const auto& peer : added) {
        added_str.append(reinterpret_cast<const char*>(peer.data()), 6);
    }
    dict->values["added"] = added_str;
    
    // Convert dropped peers to binary string
    std::string dropped_str;
    for (const auto& peer : dropped) {
        dropped_str.append(reinterpret_cast<const char*>(peer.data()), 6);
    }
    dict->values["dropped"] = dropped_str;
    
    std::string bencoded = BencodeParser::encode(std::move(dict));
    
    std::vector<uint8_t> payload;
    payload.push_back(ext_msg_id);
    payload.insert(payload.end(), bencoded.begin(), bencoded.end());
    
    return payload;
}

bool ExtensionProtocol::parse_metadata_message(
    const std::vector<uint8_t>& payload, MetadataMessage& msg) {
    
    if (payload.empty()) {
        return false;
    }
    
    try {
        std::string payload_str(payload.begin(), payload.end());
        size_t consumed = 0;
        auto val = BencodeParser::parse(payload_str, consumed);
        
        if (!std::holds_alternative<std::unique_ptr<BencodeDict>>(val)) {
            return false;
        }
        
        const auto& dict = *std::get<std::unique_ptr<BencodeDict>>(val);
        
        // Parse msg_type
        if (dict.values.count("msg_type") && std::holds_alternative<BencodeInteger>(dict.values.at("msg_type"))) {
            msg.msg_type = static_cast<uint8_t>(std::get<BencodeInteger>(dict.values.at("msg_type")));
        } else {
            return false;
        }
        
        // Parse piece
        if (dict.values.count("piece") && std::holds_alternative<BencodeInteger>(dict.values.at("piece"))) {
            msg.piece = static_cast<size_t>(std::get<BencodeInteger>(dict.values.at("piece")));
        } else {
            return false;
        }
        
        // Parse total_size (for data messages)
        if (dict.values.count("total_size") && std::holds_alternative<BencodeInteger>(dict.values.at("total_size"))) {
            msg.total_size = static_cast<size_t>(std::get<BencodeInteger>(dict.values.at("total_size")));
        }
        
        // Extract data (everything after dictionary)
        if (consumed < payload.size()) {
            msg.data.assign(payload.begin() + consumed, payload.end());
        } else {
            msg.data.clear();
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

bool ExtensionProtocol::parse_pex_message(const std::vector<uint8_t>& payload, PexMessage& msg) {
    try {
        std::string payload_str(payload.begin(), payload.end());
        auto val = BencodeParser::parse(payload_str);
        
        if (!std::holds_alternative<std::unique_ptr<BencodeDict>>(val)) {
            return false;
        }
        
        const auto& dict = *std::get<std::unique_ptr<BencodeDict>>(val);

        // Parse added peers
        if (dict.values.count("added") && std::holds_alternative<BencodeString>(dict.values.at("added"))) {
            const std::string& added_str = std::get<BencodeString>(dict.values.at("added"));
            for (size_t i = 0; i + 6 <= added_str.size(); i += 6) {
                std::array<uint8_t, 6> peer;
                std::memcpy(peer.data(), added_str.data() + i, 6);
                msg.added.push_back(peer);
            }
        }
        
        // Parse dropped peers
        if (dict.values.count("dropped") && std::holds_alternative<BencodeString>(dict.values.at("dropped"))) {
            const std::string& dropped_str = std::get<BencodeString>(dict.values.at("dropped"));
            for (size_t i = 0; i + 6 <= dropped_str.size(); i += 6) {
                std::array<uint8_t, 6> peer;
                std::memcpy(peer.data(), dropped_str.data() + i, 6);
                msg.dropped.push_back(peer);
            }
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace pixelscrape
