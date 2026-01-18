#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace pixelscrape {

struct TorrentState {
  std::string info_hash_hex;
  std::string torrent_data; // Raw bencoded torrent data (for restoration)
  std::vector<bool> bitfield;
  size_t uploaded_bytes;
  size_t downloaded_bytes;
  std::vector<size_t> file_priorities; // 0 = skip, 1 = normal, 2 = high
  std::chrono::system_clock::time_point last_updated;
};

class StateManager {
public:
  StateManager(const std::string &state_dir);

  // State management
  bool save_state(const std::string &info_hash_hex, const TorrentState &state);
  std::optional<TorrentState> load_state(const std::string &info_hash_hex);
  bool delete_state(const std::string &info_hash_hex);
  std::vector<std::string> list_saved_torrents() const;

  // Utility
  static std::string info_hash_to_hex(const std::array<uint8_t, 20> &info_hash);
  static std::array<uint8_t, 20> hex_to_info_hash(const std::string &hex);

private:
  std::string get_state_file_path(const std::string &info_hash_hex) const;

  std::string state_dir_;
};

} // namespace pixelscrape