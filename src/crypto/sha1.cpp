#include "sha1.hpp"
#include <cstring>

namespace pixelscrape {

SHA1::SHA1() {
    reset();
}

void SHA1::reset() {
    h_[0] = 0x67452301;
    h_[1] = 0xEFCDAB89;
    h_[2] = 0x98BADCFE;
    h_[3] = 0x10325476;
    h_[4] = 0xC3D2E1F0;
    length_ = 0;
    buffer_index_ = 0;
}

void SHA1::update(const uint8_t* data, size_t length) {
    length_ += length;

    while (length > 0) {
        size_t to_copy = std::min(length, static_cast<size_t>(64 - buffer_index_));
        std::memcpy(buffer_.data() + buffer_index_, data, to_copy);
        buffer_index_ += to_copy;
        data += to_copy;
        length -= to_copy;

        if (buffer_index_ == 64) {
            process_block();
            buffer_index_ = 0;
        }
    }
}

void SHA1::update(const std::string& data) {
    update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

void SHA1::update(const std::vector<uint8_t>& data) {
    update(data.data(), data.size());
}

std::array<uint8_t, SHA1::HASH_SIZE> SHA1::finalize() {
    // Append padding
    buffer_[buffer_index_++] = 0x80;

    if (buffer_index_ > 56) {
        // Not enough space for length, pad with zeros and process
        std::fill(buffer_.begin() + buffer_index_, buffer_.end(), 0);
        process_block();
        buffer_index_ = 0;
    }

    // Pad with zeros until we have space for length
    std::fill(buffer_.begin() + buffer_index_, buffer_.begin() + 56, 0);

    // Append length in bits (big-endian)
    uint64_t bit_length = length_ * 8;
    for (int i = 0; i < 8; ++i) {
        buffer_[56 + i] = (bit_length >> (56 - i * 8)) & 0xFF;
    }

    process_block();

    // Convert to big-endian byte array
    std::array<uint8_t, HASH_SIZE> result;
    for (int i = 0; i < 5; ++i) {
        result[i * 4] = (h_[i] >> 24) & 0xFF;
        result[i * 4 + 1] = (h_[i] >> 16) & 0xFF;
        result[i * 4 + 2] = (h_[i] >> 8) & 0xFF;
        result[i * 4 + 3] = h_[i] & 0xFF;
    }

    reset();
    return result;
}

void SHA1::process_block() {
    uint32_t w[80];

    // Copy buffer to w, converting to big-endian
    for (int i = 0; i < 16; ++i) {
        w[i] = (buffer_[i * 4] << 24) | (buffer_[i * 4 + 1] << 16) |
               (buffer_[i * 4 + 2] << 8) | buffer_[i * 4 + 3];
    }

    // Extend the sixteen 32-bit words into eighty 32-bit words
    for (int i = 16; i < 80; ++i) {
        w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]);
        w[i] = (w[i] << 1) | (w[i] >> 31); // Rotate left by 1
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;

        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d;
        d = c;
        c = (b << 30) | (b >> 2);
        b = a;
        a = temp;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

std::array<uint8_t, SHA1::HASH_SIZE> SHA1::hash(const uint8_t* data, size_t length) {
    SHA1 sha1;
    sha1.update(data, length);
    return sha1.finalize();
}

std::array<uint8_t, SHA1::HASH_SIZE> SHA1::hash(const std::string& data) {
    SHA1 sha1;
    sha1.update(data);
    return sha1.finalize();
}

std::array<uint8_t, SHA1::HASH_SIZE> SHA1::hash(const std::vector<uint8_t>& data) {
    SHA1 sha1;
    sha1.update(data);
    return sha1.finalize();
}

} // namespace pixelscrape