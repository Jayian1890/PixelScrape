#include "state_manager.hpp"
#include <json.hpp>
#include <filesystem.hpp>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace pixelscrape {

namespace {

std::optional<std::chrono::system_clock::time_point> parse_iso8601_utc(const std::string& value) {
    std::tm tm{};
    std::istringstream ss(value);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (ss.fail()) {
        return std::nullopt;
    }

#if defined(_WIN32)
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif

    if (t == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }

    return std::chrono::system_clock::from_time_t(t);
}

} // namespace

StateManager::StateManager(const std::string& state_dir) : state_dir_(state_dir) {
    pixellib::core::filesystem::FileSystem::create_directories(state_dir);
}

bool StateManager::save_state(const std::string& info_hash_hex, const TorrentState& state) {
    try {
        pixellib::core::json::JSON json = pixellib::core::json::JSON::object({});

        json["info_hash"] = pixellib::core::json::JSON(info_hash_hex);
        json["uploaded_bytes"] = pixellib::core::json::JSON(static_cast<double>(state.uploaded_bytes));
        json["downloaded_bytes"] = pixellib::core::json::JSON(static_cast<double>(state.downloaded_bytes));

        // Convert bitfield to array of booleans
        pixellib::core::json::JSON bitfield_array = pixellib::core::json::JSON::array({});
        for (bool bit : state.bitfield) {
            bitfield_array.push_back(pixellib::core::json::JSON(bit));
        }
        json["bitfield"] = bitfield_array;

        // Convert file priorities
        pixellib::core::json::JSON priorities_array = pixellib::core::json::JSON::array({});
        for (size_t priority : state.file_priorities) {
            priorities_array.push_back(pixellib::core::json::JSON(static_cast<double>(priority)));
        }
        json["file_priorities"] = priorities_array;

        // Convert timestamp
        auto time_t = std::chrono::system_clock::to_time_t(state.last_updated);
        std::stringstream time_ss;
        time_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
        json["last_updated"] = pixellib::core::json::JSON(time_ss.str());

        std::string json_str = json.stringify({true});
        auto file_path = get_state_file_path(info_hash_hex);

        return pixellib::core::filesystem::FileSystem::write_file(file_path, json_str);
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<TorrentState> StateManager::load_state(const std::string& info_hash_hex) {
    try {
        auto file_path = get_state_file_path(info_hash_hex);
        if (!std::filesystem::exists(file_path)) {
            return std::nullopt;
        }

        auto json_str = pixellib::core::filesystem::FileSystem::read_file(file_path);
        if (json_str.empty()) {
            return std::nullopt;
        }

        auto json_value = pixellib::core::json::JSON::parse_or_throw(json_str);
        if (!json_value.is_object()) {
            return std::nullopt;
        }

        const auto& json = json_value;
        TorrentState state;

        // Load info_hash
        if (auto it = json.find("info_hash"); it && it->is_string()) {
            state.info_hash_hex = it->as_string();
        } else {
            return std::nullopt;
        }

        // Load bitfield
        if (const auto* bitfield_ptr = json.find("bitfield"); bitfield_ptr && bitfield_ptr->is_array()) {
            const auto& bitfield_array = bitfield_ptr->as_array();
            for (const auto& bit : bitfield_array) {
                if (bit.is_bool()) {
                    state.bitfield.push_back(bit.as_bool());
                }
            }
        }

        // Load byte counts
        if (auto it = json.find("uploaded_bytes"); it && it->is_number()) {
            state.uploaded_bytes = static_cast<size_t>(it->as_number().to_int64());
        }

        if (auto it = json.find("downloaded_bytes"); it && it->is_number()) {
            state.downloaded_bytes = static_cast<size_t>(it->as_number().to_int64());
        }

        // Load file priorities
        if (const auto* priorities_ptr = json.find("file_priorities"); priorities_ptr && priorities_ptr->is_array()) {
            const auto& priorities_array = priorities_ptr->as_array();
            for (const auto& priority : priorities_array) {
                if (priority.is_number()) {
                    state.file_priorities.push_back(static_cast<size_t>(priority.as_number().to_int64()));
                }
            }
        }

        // Load timestamp
        if (auto it = json.find("last_updated"); it && it->is_string()) {
            if (auto parsed = parse_iso8601_utc(it->as_string())) {
                state.last_updated = *parsed;
            } else {
                state.last_updated = std::chrono::system_clock::now();
            }
        } else {
            state.last_updated = std::chrono::system_clock::now();
        }

        return state;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool StateManager::delete_state(const std::string& info_hash_hex) {
    try {
        auto file_path = get_state_file_path(info_hash_hex);
        if (pixellib::core::filesystem::FileSystem::exists(file_path)) {
            pixellib::core::filesystem::FileSystem::remove(file_path);
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string StateManager::get_state_file_path(const std::string& info_hash_hex) const {
    return state_dir_ + "/" + info_hash_hex + ".json";
}

std::string StateManager::info_hash_to_hex(const std::array<uint8_t, 20>& info_hash) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : info_hash) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

std::array<uint8_t, 20> StateManager::hex_to_info_hash(const std::string& hex) {
    std::array<uint8_t, 20> info_hash{};
    if (hex.length() != 40) {
        throw std::invalid_argument("Invalid info_hash hex length");
    }

    for (size_t i = 0; i < 20; ++i) {
        std::stringstream ss(hex.substr(i * 2, 2));
        int byte;
        ss >> std::hex >> byte;
        info_hash[i] = static_cast<uint8_t>(byte);
    }

    return info_hash;
}

} // namespace pixelscrape