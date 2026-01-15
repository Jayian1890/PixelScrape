#pragma once

#include <array>
#include <string>
#include <vector>
#include <cstdint>

namespace pixelscrape {

class SHA1 {
public:
    static constexpr size_t HASH_SIZE = 20;

    SHA1();
    void update(const uint8_t* data, size_t length);
    void update(const std::string& data);
    void update(const std::vector<uint8_t>& data);
    std::array<uint8_t, HASH_SIZE> finalize();

    // Convenience function for one-shot hashing
    static std::array<uint8_t, HASH_SIZE> hash(const uint8_t* data, size_t length);
    static std::array<uint8_t, HASH_SIZE> hash(const std::string& data);
    static std::array<uint8_t, HASH_SIZE> hash(const std::vector<uint8_t>& data);

private:
    void process_block();
    void reset();

    uint32_t h_[5];
    uint64_t length_;
    size_t buffer_index_;
    std::array<uint8_t, 64> buffer_;
};

} // namespace pixelscrape