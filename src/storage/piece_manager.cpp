#include "piece_manager.hpp"
#include "sha1.hpp"
#include <filesystem.hpp>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace pixelscrape {

PieceManager::PieceManager(const TorrentMetadata& metadata, const std::filesystem::path& download_dir)
    : metadata_(metadata)
    , download_dir_(download_dir)
    , have_pieces_(metadata.piece_hashes.size(), false)
    , active_downloads_(metadata.piece_hashes.size())
    , disk_running_(true)
    , disk_thread_(&PieceManager::disk_worker, this)
{
    calculate_file_mappings();

    // Create download directory structure
    pixellib::core::filesystem::FileSystem::create_directories(download_dir);

    // Create files with correct sizes
    for (const auto& file : metadata.files) {
        std::filesystem::path file_path = download_dir / file.path;
        pixellib::core::filesystem::FileSystem::create_directories(file_path.parent_path());

        // Create sparse file
        std::ofstream out_file(file_path, std::ios::binary | std::ios::out);
        if (file.length > 0) {
            out_file.seekp(file.length - 1);
            out_file.put('\0');
        }
    }
}

PieceManager::~PieceManager() {
    disk_running_ = false;
    disk_cv_.notify_one();
    if (disk_thread_.joinable()) {
        disk_thread_.join();
    }
}

size_t PieceManager::select_rarest_piece(const std::vector<std::vector<bool>>& peer_bitfields) {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    std::vector<size_t> availability(metadata_.piece_hashes.size(), 0);

    // Count how many peers have each piece
    for (const auto& bitfield : peer_bitfields) {
        if (bitfield.size() != metadata_.piece_hashes.size()) continue;
        for (size_t i = 0; i < bitfield.size(); ++i) {
            if (bitfield[i]) availability[i]++;
        }
    }

    // Find rarest piece we don't have
    size_t rarest_index = SIZE_MAX;
    size_t min_availability = SIZE_MAX;

    for (size_t i = 0; i < availability.size(); ++i) {
        if (!have_pieces_[i] && availability[i] > 0 && availability[i] < min_availability) {
            min_availability = availability[i];
            rarest_index = i;
        }
    }

    return rarest_index;
}

bool PieceManager::request_block(size_t piece_index, size_t block_index, size_t block_size) {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (piece_index >= active_downloads_.size()) return false;

    auto& download = active_downloads_[piece_index];
    if (download.blocks_received.empty()) {
        // Initialize download
        size_t piece_size = (piece_index == metadata_.piece_hashes.size() - 1) ?
            (metadata_.total_length % metadata_.piece_length) : metadata_.piece_length;
        if (piece_size == 0) piece_size = metadata_.piece_length;

        download.total_blocks = (piece_size + block_size - 1) / block_size;
        download.blocks_received.resize(download.total_blocks, false);
        download.block_data.resize(download.total_blocks);
        download.start_time = std::chrono::steady_clock::now();
    }

    if (block_index >= download.blocks_received.size()) return false;
    if (download.blocks_received[block_index]) return false; // Already received

    download.blocks_received[block_index] = true;
    return true;
}

bool PieceManager::receive_block(size_t piece_index, size_t begin, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (piece_index >= active_downloads_.size()) return false;

    auto& download = active_downloads_[piece_index];
    if (download.blocks_received.empty()) return false;

    size_t block_size = 16384; // 16KB blocks
    size_t block_index = begin / block_size;

    if (block_index >= download.blocks_received.size()) return false;
    if (download.blocks_received[block_index]) return false;

    download.blocks_received[block_index] = true;
    download.block_data[block_index] = data;

    return true;
}

bool PieceManager::is_piece_complete(size_t piece_index) const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (piece_index >= active_downloads_.size()) return false;

    const auto& download = active_downloads_[piece_index];
    if (download.blocks_received.empty()) return false;

    return std::all_of(download.blocks_received.begin(), download.blocks_received.end(),
                      [](bool received) { return received; });
}

bool PieceManager::verify_piece(size_t piece_index) {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (!is_piece_complete(piece_index)) return false;

    const auto& download = active_downloads_[piece_index];

    // Concatenate all blocks
    std::vector<uint8_t> piece_data;
    for (const auto& block : download.block_data) {
        piece_data.insert(piece_data.end(), block.begin(), block.end());
    }

    // Calculate SHA-1 hash
    auto hash = SHA1::hash(piece_data);

    // Compare with expected hash
    if (hash == metadata_.piece_hashes[piece_index]) {
        have_pieces_[piece_index] = true;
        return true;
    }

    // Verification failed, reset download
    active_downloads_[piece_index] = PieceDownload{};
    return false;
}

std::vector<uint8_t> PieceManager::read_piece(size_t piece_index) {
    // This would be used for seeding - read piece data from files
    // Implementation would reconstruct piece from file mappings
    return {};
}

bool PieceManager::write_piece(size_t piece_index, const std::vector<uint8_t>& data) {
    // Write piece data to appropriate files based on mappings
    size_t offset = 0;
    for (const auto& mapping : piece_file_mappings_[piece_index]) {
        std::filesystem::path file_path = download_dir_ / metadata_.files[mapping.file_index].path;
        std::ofstream file(file_path, std::ios::binary | std::ios::in | std::ios::out);
        if (!file) return false;

        file.seekp(metadata_.files[mapping.file_index].offset.value_or(0) + mapping.file_offset);
        file.write(reinterpret_cast<const char*>(data.data() + offset), mapping.length);
        offset += mapping.length;
    }
    return true;
}

size_t PieceManager::get_completed_pieces() const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    return std::count(have_pieces_.begin(), have_pieces_.end(), true);
}

double PieceManager::get_completion_percentage() const {
    return static_cast<double>(get_completed_pieces()) / get_total_pieces() * 100.0;
}

std::vector<bool> PieceManager::get_bitfield() const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    return have_pieces_;
}

void PieceManager::load_state(const std::vector<bool>& bitfield) {
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    if (bitfield.size() == have_pieces_.size()) {
        have_pieces_ = bitfield;
    }
}

std::vector<bool> PieceManager::save_state() const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    return have_pieces_;
}

void PieceManager::disk_worker() {
    while (disk_running_) {
        std::unique_lock<std::mutex> lock(disk_mutex_);
        disk_cv_.wait(lock, [this]() { return !disk_queue_.empty() || !disk_running_; });

        if (!disk_running_) break;

        DiskRequest request = std::move(disk_queue_.front());
        disk_queue_.pop();
        lock.unlock();

        // Process disk request
        if (request.type == DiskRequest::READ) {
            // Read operation
            auto data = read_piece(request.piece_index);
            if (request.callback) {
                request.callback(data);
            }
        } else if (request.type == DiskRequest::WRITE) {
            // Write operation
            write_piece(request.piece_index, request.data);
        }
    }
}

void PieceManager::calculate_file_mappings() {
    piece_file_mappings_.resize(metadata_.piece_hashes.size());

    size_t current_piece = 0;
    size_t piece_offset = 0;

    for (size_t file_idx = 0; file_idx < metadata_.files.size(); ++file_idx) {
        const auto& file = metadata_.files[file_idx];
        size_t file_remaining = file.length;

        while (file_remaining > 0 && current_piece < metadata_.piece_hashes.size()) {
            size_t piece_remaining = metadata_.piece_length - piece_offset;
            size_t bytes_in_this_piece = std::min(file_remaining, piece_remaining);

            piece_file_mappings_[current_piece].push_back({
                file_idx,
                file.length - file_remaining,
                bytes_in_this_piece
            });

            file_remaining -= bytes_in_this_piece;
            piece_offset += bytes_in_this_piece;

            if (piece_offset >= metadata_.piece_length) {
                current_piece++;
                piece_offset = 0;
            }
        }
    }
}

std::pair<size_t, size_t> PieceManager::get_file_range(size_t piece_index, size_t offset, size_t length) const {
    // Helper for mapping piece offsets to file ranges
    size_t global_offset = piece_index * metadata_.piece_length + offset;
    size_t end_offset = global_offset + length;

    // Find which file this range belongs to
    for (size_t i = 0; i < metadata_.files.size(); ++i) {
        const auto& file = metadata_.files[i];
        size_t file_start = file.offset.value_or(0);
        size_t file_end = file_start + file.length;

        if (global_offset >= file_start && global_offset < file_end) {
            size_t file_offset = global_offset - file_start;
            size_t file_length = std::min(length, file_end - global_offset);
            return {i, file_offset};
        }
    }

    return {SIZE_MAX, 0};
}

} // namespace pixelscrape