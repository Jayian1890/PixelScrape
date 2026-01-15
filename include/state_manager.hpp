#pragma once

#include "torrent_metadata.hpp"
#include "piece_manager.hpp"
#include <filesystem>
#include <string>

namespace pixelscrape {

struct TorrentState {
    std::string info_hash_hex;
    std::vector<bool> bitfield;
    size_t uploaded_bytes;
    size_t downloaded_bytes;
    std::vector<size_t> file_priorities; // 0 = skip, 1 = normal, 2 = high
    std::chrono::system_clock::time_point last_updated;
};

class StateManager {
public:
    StateManager(const std::filesystem::path& state_dir);

    // State management
    bool save_state(const std::string& info_hash_hex, const TorrentState& state);
    std::optional<TorrentState> load_state(const std::string& info_hash_hex);
    bool delete_state(const std::string& info_hash_hex);

    // Utility
    static std::string info_hash_to_hex(const std::array<uint8_t, 20>& info_hash);
    static std::array<uint8_t, 20> hex_to_info_hash(const std::string& hex);

private:
    std::filesystem::path get_state_file_path(const std::string& info_hash_hex) const;

    std::filesystem::path state_dir_;
};

} // namespace pixelscrape