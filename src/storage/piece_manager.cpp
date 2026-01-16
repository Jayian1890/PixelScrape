#include "piece_manager.hpp"
#include "sha1.hpp"
#include <filesystem.hpp>
#include <logging.hpp>
#include <algorithm>
#include <fstream>

namespace pixelscrape {

std::string get_parent_path(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return path.substr(0, pos);
    }
    return "";
}

PieceManager::PieceManager(const TorrentMetadata& metadata, const std::string& download_dir)
    : metadata_(metadata)
    , download_dir_(download_dir)
    , have_pieces_(metadata.piece_hashes.size(), false)
    , active_downloads_(metadata.piece_hashes.size())
    , disk_running_(true)
    , disk_thread_(&PieceManager::disk_worker, this)
{
    calculate_file_mappings();

    try {
        // Create download directory structure
        pixellib::core::filesystem::FileSystem::create_directories(download_dir);

        // Create files with correct sizes
        for (const auto& file : metadata.files) {
            std::string file_path = download_dir_ + "/" + file.path;
            std::string parent_dir = get_parent_path(file_path);
            if (!parent_dir.empty()) {
                pixellib::core::filesystem::FileSystem::create_directories(parent_dir);
            }

            // Create sparse file
            try {
                std::ofstream out_file(file_path, std::ios::binary | std::ios::out);
                if (!out_file) {
                    pixellib::core::logging::Logger::error("Failed to create file: {}", file_path);
                    throw std::runtime_error("Failed to create file: " + file_path);
                }
                
                if (file.length > 0) {
                    out_file.seekp(file.length - 1);
                    out_file.put('\0');
                    if (!out_file) {
                        pixellib::core::logging::Logger::error("Failed to write sparse file: {}", file_path);
                        throw std::runtime_error("Failed to write sparse file: " + file_path);
                    }
                }
                out_file.close();
            } catch (const std::exception& e) {
                pixellib::core::logging::Logger::error("Exception creating file {}: {}", file_path, e.what());
                throw;
            }
        }
    } catch (const std::exception& e) {
        pixellib::core::logging::Logger::error("Failed to initialize PieceManager: {}", e.what());
        throw;
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
        download.blocks_requested.resize(download.total_blocks, false);
        download.blocks_received.resize(download.total_blocks, false);
        download.block_data.resize(download.total_blocks);
        download.start_time = std::chrono::steady_clock::now();
    }

    if (block_index >= download.blocks_requested.size()) return false;
    if (download.blocks_requested[block_index]) return false; // Already requested

    download.blocks_requested[block_index] = true;
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
    if (piece_index >= have_pieces_.size() || !have_pieces_[piece_index]) {
        return {};
    }

    try {
        // Calculate piece size
        size_t piece_size = (piece_index == metadata_.piece_hashes.size() - 1) ?
            (metadata_.total_length % metadata_.piece_length) : metadata_.piece_length;
        if (piece_size == 0) piece_size = metadata_.piece_length;

        std::vector<uint8_t> piece_data(piece_size);

        // Read from file mappings
        size_t offset = 0;
        for (const auto& mapping : piece_file_mappings_[piece_index]) {
            std::string file_path = download_dir_ + "/" + metadata_.files[mapping.file_index].path;
            std::ifstream file(file_path, std::ios::binary);
            if (!file) {
                pixellib::core::logging::Logger::error("Failed to open file for reading: {}", file_path);
                return {};
            }

            file.seekg(mapping.file_offset);
            if (!file) {
                pixellib::core::logging::Logger::error("Failed to seek in file: {}", file_path);
                return {};
            }

            file.read(reinterpret_cast<char*>(piece_data.data() + offset), mapping.length);
            if (file.gcount() != static_cast<std::streamsize>(mapping.length)) {
                pixellib::core::logging::Logger::error("Failed to read complete block from file: {}", file_path);
                return {};
            }
            offset += mapping.length;
        }

        return piece_data;
    } catch (const std::exception& e) {
        pixellib::core::logging::Logger::error("Exception reading piece {}: {}", piece_index, e.what());
        return {};
    }
}

bool PieceManager::write_piece(size_t piece_index, const std::vector<uint8_t>& data) {
    try {
        // Write piece data to appropriate files based on mappings
        size_t offset = 0;
        for (const auto& mapping : piece_file_mappings_[piece_index]) {
            std::string file_path = download_dir_ + "/" + metadata_.files[mapping.file_index].path;
            std::ofstream file(file_path, std::ios::binary | std::ios::in | std::ios::out);
            if (!file) {
                pixellib::core::logging::Logger::error("Failed to open file for writing: {}", file_path);
                return false;
            }

            file.seekp(mapping.file_offset);
            if (!file) {
                pixellib::core::logging::Logger::error("Failed to seek in file for writing: {}", file_path);
                return false;
            }

            file.write(reinterpret_cast<const char*>(data.data() + offset), mapping.length);
            if (!file) {
                pixellib::core::logging::Logger::error("Failed to write to file: {}", file_path);
                return false;
            }
            offset += mapping.length;
        }
        return true;
    } catch (const std::exception& e) {
        pixellib::core::logging::Logger::error("Exception writing piece {}: {}", piece_index, e.what());
        return false;
    }
}

size_t PieceManager::get_completed_pieces() const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    return std::count(have_pieces_.begin(), have_pieces_.end(), true);
}

double PieceManager::get_completion_percentage() const {
    if (metadata_.total_length == 0) return 0.0;
    return static_cast<double>(get_total_downloaded_bytes()) / metadata_.total_length * 100.0;
}

size_t PieceManager::get_total_downloaded_bytes() const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    size_t total = 0;
    
    // Completed pieces
    for (size_t i = 0; i < have_pieces_.size(); ++i) {
        if (have_pieces_[i]) {
            size_t piece_size = (i == have_pieces_.size() - 1) ? 
                (metadata_.total_length - (have_pieces_.size() - 1) * metadata_.piece_length) :
                metadata_.piece_length;
            total += piece_size;
        }
    }

    // Partially downloaded pieces
    for (const auto& download : active_downloads_) {
        for (const auto& block : download.block_data) {
            total += block.size();
        }
    }

    return total;
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

        if (disk_queue_.empty()) continue;

        DiskRequest request = std::move(disk_queue_.front());
        disk_queue_.pop();
        lock.unlock();

        try {
            // Process disk request
            if (request.type == DiskRequest::READ) {
                // Read operation
                auto data = read_piece(request.piece_index);
                if (request.callback) {
                    request.callback(data);
                }
            } else if (request.type == DiskRequest::WRITE) {
                // Write operation
                if (!write_piece(request.piece_index, request.data)) {
                    pixellib::core::logging::Logger::error("Failed to write piece {} to disk", request.piece_index);
                }
            }
        } catch (const std::exception& e) {
            pixellib::core::logging::Logger::error("Exception in disk_worker for piece {}: {}", request.piece_index, e.what());
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
    (void)length;

    // Find which file this range belongs to
    for (size_t i = 0; i < metadata_.files.size(); ++i) {
        const auto& file = metadata_.files[i];
        size_t file_start = file.offset.value_or(0);
        size_t file_end = file_start + file.length;

        if (global_offset >= file_start && global_offset < file_end) {
            size_t file_offset = global_offset - file_start;
            return {i, file_offset};
        }
    }

    return {SIZE_MAX, 0};
}

} // namespace pixelscrape