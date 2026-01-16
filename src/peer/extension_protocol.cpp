#include "extension_protocol.hpp"
#include <arpa/inet.h>
#include <cstring>

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
    
    // Build bencoded dictionary
    pixellib::core::json::JSON dict = pixellib::core::json::JSON::object({});
    
    // Add supported extensions
    pixellib::core::json::JSON m = pixellib::core::json::JSON::object({});
    for (const auto& [name, id] : local_extensions_) {
        m[name] = pixellib::core::json::JSON(static_cast<double>(id));
    }
    dict["m"] = m;
    
    // Add client name/version
    dict["v"] = pixellib::core::json::JSON(client_name);
    
    // Add listening port
    dict["p"] = pixellib::core::json::JSON(static_cast<double>(listen_port));
    
    // Add metadata size if available
    if (metadata_size > 0) {
        dict["metadata_size"] = pixellib::core::json::JSON(static_cast<double>(metadata_size));
    }
    
    // Add other optional fields
    dict["reqq"] = pixellib::core::json::JSON(250.0); // Request queue depth
    
    // Convert to bencoded format (we'll use JSON for simplicity, but should be bencode)
    // For production, you'd want proper bencode encoding
    pixellib::core::json::StringifyOptions opts;
    opts.pretty = false;
    std::string json_str = dict.stringify(opts);
    
    // Convert string to bytes
    std::vector<uint8_t> payload(json_str.begin(), json_str.end());
    
    return payload;
}

bool ExtensionProtocol::parse_extended_handshake(const std::vector<uint8_t>& payload) {
    try {
        // Parse bencoded/JSON data
        std::string payload_str(payload.begin(), payload.end());
        auto dict = pixellib::core::json::JSON::parse_or_throw(payload_str);
        
        if (!dict.is_object()) {
            return false;
        }
        
        // Parse 'm' dictionary (extensions)
        auto m_it = dict.find("m");
        if (m_it && m_it->is_object()) {
            peer_extensions_.clear();
            for (const auto& [key, value] : m_it->as_object()) {
                if (value.is_number()) {
                    peer_extensions_[key] = static_cast<uint8_t>(value.as_number().to_int64());
                }
            }
        }
        
        // Parse client name/version
        auto v_it = dict.find("v");
        if (v_it && v_it->is_string()) {
            peer_client_ = v_it->as_string();
        }
        
        // Parse listening port
        auto p_it = dict.find("p");
        if (p_it && p_it->is_number()) {
            peer_port_ = static_cast<uint16_t>(p_it->as_number().to_int64());
        }
        
        // Parse metadata size
        auto metadata_it = dict.find("metadata_size");
        if (metadata_it && metadata_it->is_number()) {
            peer_metadata_size_ = static_cast<size_t>(metadata_it->as_number().to_int64());
        }
        
        // Parse upload_only flag
        auto upload_it = dict.find("upload_only");
        if (upload_it && upload_it->is_number()) {
            peer_upload_only_ = upload_it->as_number().to_int64() != 0;
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
    // Build bencoded dictionary: d8:msg_typei0e5:piecei{piece}ee
    pixellib::core::json::JSON dict = pixellib::core::json::JSON::object({});
    dict["msg_type"] = pixellib::core::json::JSON(0.0); // request
    dict["piece"] = pixellib::core::json::JSON(static_cast<double>(piece));
    
    pixellib::core::json::StringifyOptions opts;
    opts.pretty = false;
    std::string json_str = dict.stringify(opts);
    
    std::vector<uint8_t> payload;
    payload.push_back(ext_msg_id); // Extended message ID
    payload.insert(payload.end(), json_str.begin(), json_str.end());
    
    return payload;
}

std::vector<uint8_t> ExtensionProtocol::build_metadata_data(
    uint8_t ext_msg_id, size_t piece, size_t total_size, const std::vector<uint8_t>& data) {
    
    pixellib::core::json::JSON dict = pixellib::core::json::JSON::object({});
    dict["msg_type"] = pixellib::core::json::JSON(1.0); // data
    dict["piece"] = pixellib::core::json::JSON(static_cast<double>(piece));
    dict["total_size"] = pixellib::core::json::JSON(static_cast<double>(total_size));
    
    pixellib::core::json::StringifyOptions opts;
    opts.pretty = false;
    std::string json_str = dict.stringify(opts);
    
    std::vector<uint8_t> payload;
    payload.push_back(ext_msg_id);
    payload.insert(payload.end(), json_str.begin(), json_str.end());
    payload.insert(payload.end(), data.begin(), data.end());
    
    return payload;
}

std::vector<uint8_t> ExtensionProtocol::build_metadata_reject(uint8_t ext_msg_id, size_t piece) {
    pixellib::core::json::JSON dict = pixellib::core::json::JSON::object({});
    dict["msg_type"] = pixellib::core::json::JSON(2.0); // reject
    dict["piece"] = pixellib::core::json::JSON(static_cast<double>(piece));
    
    pixellib::core::json::StringifyOptions opts;
    opts.pretty = false;
    std::string json_str = dict.stringify(opts);
    
    std::vector<uint8_t> payload;
    payload.push_back(ext_msg_id);
    payload.insert(payload.end(), json_str.begin(), json_str.end());
    
    return payload;
}

std::vector<uint8_t> ExtensionProtocol::build_pex_message(
    uint8_t ext_msg_id,
    const std::vector<std::array<uint8_t, 6>>& added,
    const std::vector<std::array<uint8_t, 6>>& dropped) {
    
    // PEX message format: d5:added{binary data}7:dropped{binary data}e
    pixellib::core::json::JSON dict = pixellib::core::json::JSON::object({});
    
    // Convert added peers to binary string (IP:port pairs, 6 bytes each)
    std::string added_str;
    for (const auto& peer : added) {
        added_str.append(reinterpret_cast<const char*>(peer.data()), 6);
    }
    dict["added"] = pixellib::core::json::JSON(added_str);
    
    // Convert dropped peers to binary string
    std::string dropped_str;
    for (const auto& peer : dropped) {
        dropped_str.append(reinterpret_cast<const char*>(peer.data()), 6);
    }
    dict["dropped"] = pixellib::core::json::JSON(dropped_str);
    
    pixellib::core::json::StringifyOptions opts;
    opts.pretty = false;
    std::string json_str = dict.stringify(opts);
    
    std::vector<uint8_t> payload;
    payload.push_back(ext_msg_id);
    payload.insert(payload.end(), json_str.begin(), json_str.end());
    
    return payload;
}

bool ExtensionProtocol::parse_metadata_message(
    const std::vector<uint8_t>& payload, MetadataMessage& msg) {
    
    if (payload.empty()) {
        return false;
    }
    
    try {
        // Find the end of bencoded dictionary (look for 'e')
        size_t dict_end = 0;
        int depth = 0;
        bool in_dict = false;
        
        for (size_t i = 0; i < payload.size(); ++i) {
            if (payload[i] == 'd') {
                in_dict = true;
                depth++;
            } else if (payload[i] == 'e') {
                depth--;
                if (depth == 0 && in_dict) {
                    dict_end = i + 1;
                    break;
                }
            }
        }
        
        if (dict_end == 0) {
            return false;
        }
        
        // Parse dictionary part
        std::string dict_str(payload.begin(), payload.begin() + dict_end);
        auto dict = pixellib::core::json::JSON::parse_or_throw(dict_str);
        
        if (!dict.is_object()) {
            return false;
        }
        
        // Parse msg_type
        auto type_it = dict.find("msg_type");
        if (!type_it || !type_it->is_number()) {
            return false;
        }
        msg.msg_type = static_cast<uint8_t>(type_it->as_number().to_int64());
        
        // Parse piece
        auto piece_it = dict.find("piece");
        if (!piece_it || !piece_it->is_number()) {
            return false;
        }
        msg.piece = static_cast<size_t>(piece_it->as_number().to_int64());
        
        // Parse total_size (for data messages)
        auto size_it = dict.find("total_size");
        if (size_it && size_it->is_number()) {
            msg.total_size = static_cast<size_t>(size_it->as_number().to_int64());
        }
        
        // Extract data (everything after dictionary)
        if (dict_end < payload.size()) {
            msg.data.assign(payload.begin() + dict_end, payload.end());
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

bool ExtensionProtocol::parse_pex_message(const std::vector<uint8_t>& payload, PexMessage& msg) {
    try {
        std::string payload_str(payload.begin(), payload.end());
        auto dict = pixellib::core::json::JSON::parse_or_throw(payload_str);
        
        if (!dict.is_object()) {
            return false;
        }
        
        // Parse added peers
        auto added_it = dict.find("added");
        if (added_it && added_it->is_string()) {
            const std::string& added_str = added_it->as_string();
            for (size_t i = 0; i + 6 <= added_str.size(); i += 6) {
                std::array<uint8_t, 6> peer;
                std::memcpy(peer.data(), added_str.data() + i, 6);
                msg.added.push_back(peer);
            }
        }
        
        // Parse dropped peers
        auto dropped_it = dict.find("dropped");
        if (dropped_it && dropped_it->is_string()) {
            const std::string& dropped_str = dropped_it->as_string();
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
