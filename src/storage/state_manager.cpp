#include "state_manager.hpp"
#include <json.hpp>
#include <filesystem.hpp>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <filesystem.hpp>
#include <iomanip>
#include <json.hpp>
#include <sstream>

namespace pixelscrape {

namespace {

// Base64 encoding/decoding for torrent data
static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                        "abcdefghijklmnopqrstuvwxyz"
                                        "0123456789+/";

std::string base64_encode(const std::string &input) {
  std::string ret;
  int i = 0;
  unsigned char char_array_3[3];
  unsigned char char_array_4[4];
  size_t in_len = input.size();
  const unsigned char *bytes_to_encode =
      reinterpret_cast<const unsigned char *>(input.data());

  while (in_len--) {
    char_array_3[i++] = *(bytes_to_encode++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] =
          ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] =
          ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;

      for (i = 0; i < 4; i++)
        ret += base64_chars[char_array_4[i]];
      i = 0;
    }
  }

  if (i) {
    for (int j = i; j < 3; j++)
      char_array_3[j] = '\0';

    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] =
        ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] =
        ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

    for (int j = 0; j < i + 1; j++)
      ret += base64_chars[char_array_4[j]];

    while (i++ < 3)
      ret += '=';
  }

  return ret;
}

std::string base64_decode(const std::string &encoded) {
  std::vector<int> T(256, -1);
  for (int i = 0; i < 64; i++)
    T[base64_chars[i]] = i;

  std::string out;
  int val = 0, valb = -8;
  for (unsigned char c : encoded) {
    if (T[c] == -1)
      continue;
    val = (val << 6) + T[c];
    valb += 6;
    if (valb >= 0) {
      out.push_back(char((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

std::optional<std::chrono::system_clock::time_point>
parse_iso8601_utc(const std::string &value) {
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

StateManager::StateManager(const std::string &state_dir)
    : state_dir_(state_dir) {
  pixellib::core::filesystem::FileSystem::create_directories(state_dir);
}

bool StateManager::save_state(const std::string &info_hash_hex,
                              const TorrentState &state) {
  try {
    pixellib::core::json::JSON json = pixellib::core::json::JSON::object({});

    json["info_hash"] = pixellib::core::json::JSON(info_hash_hex);
    json["uploaded_bytes"] =
        pixellib::core::json::JSON(static_cast<double>(state.uploaded_bytes));
    json["downloaded_bytes"] =
        pixellib::core::json::JSON(static_cast<double>(state.downloaded_bytes));

    // Store torrent data as base64
    if (!state.torrent_data.empty()) {
      json["torrent_data"] =
          pixellib::core::json::JSON(base64_encode(state.torrent_data));
    }

    // Convert bitfield to array of booleans
    pixellib::core::json::JSON bitfield_array =
        pixellib::core::json::JSON::array({});
    for (bool bit : state.bitfield) {
      bitfield_array.push_back(pixellib::core::json::JSON(bit));
    }
    json["bitfield"] = bitfield_array;

    // Convert file priorities
    pixellib::core::json::JSON priorities_array =
        pixellib::core::json::JSON::array({});
    for (size_t priority : state.file_priorities) {
      priorities_array.push_back(
          pixellib::core::json::JSON(static_cast<double>(priority)));
    }
    json["file_priorities"] = priorities_array;

    // Convert timestamp
    auto time_t = std::chrono::system_clock::to_time_t(state.last_updated);
    std::stringstream time_ss;
    time_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    json["last_updated"] = pixellib::core::json::JSON(time_ss.str());

    std::string json_str = json.stringify({true});
    auto file_path = get_state_file_path(info_hash_hex);

    return pixellib::core::filesystem::FileSystem::write_file(file_path,
                                                              json_str);
  } catch (const std::exception &) {
    return false;
  }
}

std::optional<TorrentState>
StateManager::load_state(const std::string &info_hash_hex) {
  try {
    auto file_path = get_state_file_path(info_hash_hex);
    if (!pixellib::core::filesystem::FileSystem::exists(file_path)) {
      return std::nullopt;
    }

    auto json_str =
        pixellib::core::filesystem::FileSystem::read_file(file_path);
    if (json_str.empty()) {
      return std::nullopt;
    }

    auto json_value = pixellib::core::json::JSON::parse_or_throw(json_str);
    if (!json_value.is_object()) {
      return std::nullopt;
    }

    const auto &json = json_value;
    TorrentState state;

    // Load info_hash
    if (auto it = json.find("info_hash"); it && it->is_string()) {
      state.info_hash_hex = it->as_string();
    } else {
      return std::nullopt;
    }

    // Load bitfield
    if (const auto *bitfield_ptr = json.find("bitfield");
        bitfield_ptr && bitfield_ptr->is_array()) {
      const auto &bitfield_array = bitfield_ptr->as_array();
      for (const auto &bit : bitfield_array) {
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

    // Load torrent data
    if (auto it = json.find("torrent_data"); it && it->is_string()) {
      state.torrent_data = base64_decode(it->as_string());
    }

    // Load file priorities
    if (const auto *priorities_ptr = json.find("file_priorities");
        priorities_ptr && priorities_ptr->is_array()) {
      const auto &priorities_array = priorities_ptr->as_array();
      for (const auto &priority : priorities_array) {
        if (priority.is_number()) {
          state.file_priorities.push_back(
              static_cast<size_t>(priority.as_number().to_int64()));
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
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

bool StateManager::delete_state(const std::string &info_hash_hex) {
  try {
    auto file_path = get_state_file_path(info_hash_hex);
    if (pixellib::core::filesystem::FileSystem::exists(file_path)) {
      pixellib::core::filesystem::FileSystem::remove(file_path);
    }
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

std::string
StateManager::get_state_file_path(const std::string &info_hash_hex) const {
  return state_dir_ + "/" + info_hash_hex + ".json";
}

std::string
StateManager::info_hash_to_hex(const std::array<uint8_t, 20> &info_hash) {
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (uint8_t byte : info_hash) {
    ss << std::setw(2) << static_cast<int>(byte);
  }
  return ss.str();
}

std::array<uint8_t, 20> StateManager::hex_to_info_hash(const std::string &hex) {
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

std::vector<std::string> StateManager::list_saved_torrents() const {
  std::vector<std::string> info_hashes;
  try {
    auto entries =
        pixellib::core::filesystem::FileSystem::directory_iterator(state_dir_);
    for (const auto &entry : entries) {
      // Look for .json files with 40-character hex names (info hash)
      if (entry.size() == 45 && entry.substr(40) == ".json") {
        std::string info_hash_hex = entry.substr(0, 40);
        // Validate it's a hex string
        bool valid = true;
        for (char c : info_hash_hex) {
          if (!std::isxdigit(static_cast<unsigned char>(c))) {
            valid = false;
            break;
          }
        }
        if (valid) {
          info_hashes.push_back(info_hash_hex);
        }
      }
    }
  } catch (const std::exception &) {
    // Ignore errors, return empty list
  }
  return info_hashes;
}

} // namespace pixelscrape