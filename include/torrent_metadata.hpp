#pragma once

#include "bencode_parser.hpp"
#include "sha1.hpp"
#include <vector>
#include <string>
#include <filesystem>
#include <optional>

namespace pixelscrape {

struct TorrentFile {
    std::string path;
    size_t length;
    std::optional<size_t> offset; // Offset within the torrent for multi-file
};

struct TorrentMetadata {
    std::string announce;
    std::array<uint8_t, SHA1::HASH_SIZE> info_hash;
    std::string name;
    size_t piece_length;
    std::vector<std::array<uint8_t, SHA1::HASH_SIZE>> piece_hashes;
    size_t total_length;
    std::vector<TorrentFile> files;

    // For single-file torrents
    bool is_single_file() const { return files.size() == 1; }

    // Get file at global offset
    const TorrentFile* get_file_at_offset(size_t offset) const;
};

class TorrentMetadataParser {
public:
    static TorrentMetadata parse(const std::filesystem::path& torrent_path);

private:
    static TorrentMetadata parse_from_bencode(const BencodeDict& dict);
    static std::vector<TorrentFile> parse_files(const BencodeValue& files_value, const std::string& name);
    static std::vector<std::array<uint8_t, SHA1::HASH_SIZE>> parse_pieces(const BencodeString& pieces_data);
};

} // namespace pixelscrape