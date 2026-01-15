#pragma once

#include "torrent_metadata.hpp"
#include "peer_connection.hpp"
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <filesystem>

namespace pixelscrape {

struct PieceDownload {
    size_t index;
    std::vector<bool> blocks_received;
    std::vector<std::vector<uint8_t>> block_data;
    size_t total_blocks;
    std::chrono::steady_clock::time_point start_time;
};

struct DiskRequest {
    enum Type { READ, WRITE };
    Type type;
    size_t piece_index;
    size_t offset;
    std::vector<uint8_t> data;
    std::function<void(const std::vector<uint8_t>&)> callback;
};

class PieceManager {
public:
    PieceManager(const TorrentMetadata& metadata, const std::filesystem::path& download_dir);
    ~PieceManager();

    // Piece management
    size_t select_rarest_piece(const std::vector<std::vector<bool>>& peer_bitfields);
    bool request_block(size_t piece_index, size_t block_index, size_t block_size);
    bool receive_block(size_t piece_index, size_t begin, const std::vector<uint8_t>& data);
    bool is_piece_complete(size_t piece_index) const;
    bool verify_piece(size_t piece_index);

    // File I/O
    std::vector<uint8_t> read_piece(size_t piece_index);
    bool write_piece(size_t piece_index, const std::vector<uint8_t>& data);

    // Statistics
    size_t get_completed_pieces() const;
    size_t get_total_pieces() const { return metadata_.piece_hashes.size(); }
    double get_completion_percentage() const;

    // State management
    std::vector<bool> get_bitfield() const;
    void load_state(const std::vector<bool>& bitfield);
    std::vector<bool> save_state() const;

private:
    void disk_worker();
    void calculate_file_mappings();
    std::pair<size_t, size_t> get_file_range(size_t piece_index, size_t offset, size_t length) const;

    const TorrentMetadata& metadata_;
    std::filesystem::path download_dir_;

    // Piece state
    std::vector<bool> have_pieces_;
    std::vector<PieceDownload> active_downloads_;
    mutable std::mutex pieces_mutex_;

    // File mappings for multi-file torrents
    struct FileMapping {
        size_t file_index;
        size_t file_offset;
        size_t length;
    };
    std::vector<std::vector<FileMapping>> piece_file_mappings_;

    // Disk I/O
    std::queue<DiskRequest> disk_queue_;
    std::mutex disk_mutex_;
    std::condition_variable disk_cv_;
    std::thread disk_thread_;
    std::atomic<bool> disk_running_;
};

} // namespace pixelscrape