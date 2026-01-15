#include "torrent_metadata.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>

namespace pixelscrape {

TorrentMetadata TorrentMetadataParser::parse(const std::filesystem::path& torrent_path) {
    std::ifstream file(torrent_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open torrent file: " + torrent_path.string());
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string data = buffer.str();

    auto bencode_value = BencodeParser::parse(data);
    if (!std::holds_alternative<std::unique_ptr<BencodeDict>>(bencode_value)) {
        throw std::runtime_error("Invalid torrent file: root must be a dictionary");
    }

    return parse_from_bencode(*std::get<std::unique_ptr<BencodeDict>>(bencode_value));
}

TorrentMetadata TorrentMetadataParser::parse(const std::string& data) {
    auto bencode_value = BencodeParser::parse(data);
    if (!std::holds_alternative<std::unique_ptr<BencodeDict>>(bencode_value)) {
        throw std::runtime_error("Invalid torrent data: root must be a dictionary");
    }

    return parse_from_bencode(*std::get<std::unique_ptr<BencodeDict>>(bencode_value));
}

TorrentMetadata TorrentMetadataParser::parse_from_bencode(const BencodeDict& dict) {
    TorrentMetadata metadata;

    // Extract announce URL
    auto announce_it = dict.values.find("announce");
    if (announce_it == dict.values.end() || !std::holds_alternative<BencodeString>(announce_it->second)) {
        throw std::runtime_error("Missing or invalid announce URL");
    }
    metadata.announce = std::get<BencodeString>(announce_it->second);

    // Extract announce-list
    auto announce_list_it = dict.values.find("announce-list");
    if (announce_list_it != dict.values.end() && std::holds_alternative<std::unique_ptr<BencodeList>>(announce_list_it->second)) {
        const auto& tiers_list = *std::get<std::unique_ptr<BencodeList>>(announce_list_it->second);
        for (const auto& tier_value : tiers_list.items) {
            if (std::holds_alternative<std::unique_ptr<BencodeList>>(tier_value)) {
                std::vector<std::string> tier;
                const auto& urls_list = *std::get<std::unique_ptr<BencodeList>>(tier_value);
                for (const auto& url_value : urls_list.items) {
                    if (std::holds_alternative<BencodeString>(url_value)) {
                        tier.push_back(std::get<BencodeString>(url_value));
                    }
                }
                if (!tier.empty()) {
                    metadata.announce_list.push_back(tier);
                }
            }
        }
    }

    // Default announce list if empty
    if (metadata.announce_list.empty()) {
        metadata.announce_list.push_back({metadata.announce});
    }

    // Extract info dictionary
    auto info_it = dict.values.find("info");
    if (info_it == dict.values.end() || !std::holds_alternative<std::unique_ptr<BencodeDict>>(info_it->second)) {
        throw std::runtime_error("Missing or invalid info dictionary");
    }
    const auto& info_dict = *std::get<std::unique_ptr<BencodeDict>>(info_it->second);

    // Calculate info_hash
    std::string info_bencoded = BencodeParser::encode(info_it->second);
    metadata.info_hash = SHA1::hash(info_bencoded);

    // Extract name
    auto name_it = info_dict.values.find("name");
    if (name_it == info_dict.values.end() || !std::holds_alternative<BencodeString>(name_it->second)) {
        throw std::runtime_error("Missing or invalid name");
    }
    metadata.name = std::get<BencodeString>(name_it->second);

    // Extract piece length
    auto piece_length_it = info_dict.values.find("piece length");
    if (piece_length_it == info_dict.values.end() || !std::holds_alternative<BencodeInteger>(piece_length_it->second)) {
        throw std::runtime_error("Missing or invalid piece length");
    }
    metadata.piece_length = static_cast<size_t>(std::get<BencodeInteger>(piece_length_it->second));

    // Extract pieces
    auto pieces_it = info_dict.values.find("pieces");
    if (pieces_it == info_dict.values.end() || !std::holds_alternative<BencodeString>(pieces_it->second)) {
        throw std::runtime_error("Missing or invalid pieces");
    }
    metadata.piece_hashes = parse_pieces(std::get<BencodeString>(pieces_it->second));

    // Extract files (single or multi-file)
    auto length_it = info_dict.values.find("length");
    auto files_it = info_dict.values.find("files");

    if (length_it != info_dict.values.end() && std::holds_alternative<BencodeInteger>(length_it->second)) {
        // Single file torrent
        size_t length = static_cast<size_t>(std::get<BencodeInteger>(length_it->second));
        metadata.files = {TorrentFile{metadata.name, length, 0}};
        metadata.total_length = length;
    } else if (files_it != info_dict.values.end() && std::holds_alternative<std::unique_ptr<BencodeList>>(files_it->second)) {
        // Multi-file torrent
        metadata.files = parse_files(files_it->second, metadata.name);
        metadata.total_length = 0;
        for (const auto& file : metadata.files) {
            metadata.total_length += file.length;
        }
    } else {
        throw std::runtime_error("Invalid torrent: must have either 'length' or 'files'");
    }

    return metadata;
}

std::vector<TorrentFile> TorrentMetadataParser::parse_files(const BencodeValue& files_value, const std::string& name) {
    const auto& files_list = *std::get<std::unique_ptr<BencodeList>>(files_value);
    std::vector<TorrentFile> files;
    size_t current_offset = 0;

    for (const auto& file_value : files_list.items) {
        if (!std::holds_alternative<std::unique_ptr<BencodeDict>>(file_value)) {
            throw std::runtime_error("Invalid file entry: must be a dictionary");
        }

        const auto& file_dict = *std::get<std::unique_ptr<BencodeDict>>(file_value);

        // Extract length
        auto length_it = file_dict.values.find("length");
        if (length_it == file_dict.values.end() || !std::holds_alternative<BencodeInteger>(length_it->second)) {
            throw std::runtime_error("Missing or invalid file length");
        }
        size_t length = static_cast<size_t>(std::get<BencodeInteger>(length_it->second));

        // Extract path
        auto path_it = file_dict.values.find("path");
        if (path_it == file_dict.values.end() || !std::holds_alternative<std::unique_ptr<BencodeList>>(path_it->second)) {
            throw std::runtime_error("Missing or invalid file path");
        }

        const auto& path_list = *std::get<std::unique_ptr<BencodeList>>(path_it->second);
        std::filesystem::path file_path = name; // Start with torrent name as root

        for (const auto& path_component : path_list.items) {
            if (!std::holds_alternative<BencodeString>(path_component)) {
                throw std::runtime_error("Invalid path component");
            }
            file_path /= std::get<BencodeString>(path_component);
        }

        files.push_back(TorrentFile{file_path.string(), length, current_offset});
        current_offset += length;
    }

    return files;
}

std::vector<std::array<uint8_t, SHA1::HASH_SIZE>> TorrentMetadataParser::parse_pieces(const BencodeString& pieces_data) {
    if (pieces_data.size() % SHA1::HASH_SIZE != 0) {
        throw std::runtime_error("Invalid pieces data length");
    }

    size_t num_pieces = pieces_data.size() / SHA1::HASH_SIZE;
    std::vector<std::array<uint8_t, SHA1::HASH_SIZE>> hashes;

    for (size_t i = 0; i < num_pieces; ++i) {
        std::array<uint8_t, SHA1::HASH_SIZE> hash;
        std::memcpy(hash.data(), pieces_data.data() + i * SHA1::HASH_SIZE, SHA1::HASH_SIZE);
        hashes.push_back(hash);
    }

    return hashes;
}

const TorrentFile* TorrentMetadata::get_file_at_offset(size_t offset) const {
    for (const auto& file : files) {
        if (file.offset && offset >= *file.offset && offset < *file.offset + file.length) {
            return &file;
        }
    }
    return nullptr;
}

} // namespace pixelscrape