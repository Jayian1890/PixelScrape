#include <vector>
#include <cstdint>
#include <arpa/inet.h>
#include <chrono>
#include <iostream>
#include <numeric>
#include <cstring>

enum class PeerMessageType : uint8_t {
  CHOKE = 0,
  UNCHOKE = 1,
  INTERESTED = 2,
  NOT_INTERESTED = 3,
  HAVE = 4,
  BITFIELD = 5,
  REQUEST = 6,
  PIECE = 7,
  CANCEL = 8,
  PORT = 9,
  KEEP_ALIVE = 10
};

struct PeerMessage {
  PeerMessageType type;
  std::vector<uint8_t> payload;
};

// Original implementation
std::vector<uint8_t> serialize_message_original(const PeerMessage &message) {
  std::vector<uint8_t> data;

  // Add payload length (4 bytes, big-endian)
  uint32_t length = 1 + message.payload.size(); // +1 for message type
  uint32_t length_be = htonl(length);
  data.insert(data.end(), reinterpret_cast<uint8_t *>(&length_be),
              reinterpret_cast<uint8_t *>(&length_be) + 4);

  // Add message type
  data.push_back(static_cast<uint8_t>(message.type));

  // Add payload
  data.insert(data.end(), message.payload.begin(), message.payload.end());

  return data;
}

// Optimized implementation
std::vector<uint8_t> serialize_message_optimized(const PeerMessage &message) {
  std::vector<uint8_t> data;
  data.reserve(4 + 1 + message.payload.size());

  // Add payload length (4 bytes, big-endian)
  uint32_t length = 1 + message.payload.size(); // +1 for message type
  uint32_t length_be = htonl(length);
  const uint8_t* len_bytes = reinterpret_cast<const uint8_t*>(&length_be);
  data.insert(data.end(), len_bytes, len_bytes + 4);

  // Add message type
  data.push_back(static_cast<uint8_t>(message.type));

  // Add payload
  data.insert(data.end(), message.payload.begin(), message.payload.end());

  return data;
}

// Optimized implementation 2 (resize + memcpy)
std::vector<uint8_t> serialize_message_optimized_2(const PeerMessage &message) {
  std::vector<uint8_t> data;
  uint32_t payload_len = message.payload.size();
  data.resize(4 + 1 + payload_len);

  // Add payload length (4 bytes, big-endian)
  uint32_t length = 1 + payload_len; // +1 for message type
  uint32_t length_be = htonl(length);
  std::memcpy(data.data(), &length_be, 4);

  // Add message type
  data[4] = static_cast<uint8_t>(message.type);

  // Add payload
  std::memcpy(data.data() + 5, message.payload.data(), payload_len);

  return data;
}

int main() {
    // Create a large message (e.g., a PIECE message)
    // Piece size is typically 16KB or more. Let's use 16KB (16 * 1024 bytes).
    const size_t payload_size = 16 * 1024;
    std::vector<uint8_t> payload(payload_size, 0xAB);
    PeerMessage message{PeerMessageType::PIECE, payload};

    const int iterations = 100000;

    // Benchmark Original
    auto start = std::chrono::high_resolution_clock::now();
    size_t total_size = 0;
    for (int i = 0; i < iterations; ++i) {
        auto data = serialize_message_original(message);
        total_size += data.size();
        // Prevent optimization
        asm volatile("" : : "g"(data.data()) : "memory");
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed_original = end - start;
    std::cout << "Original: " << elapsed_original.count() << " ms" << std::endl;

    // Benchmark Optimized
    start = std::chrono::high_resolution_clock::now();
    size_t total_size_opt = 0;
    for (int i = 0; i < iterations; ++i) {
        auto data = serialize_message_optimized(message);
        total_size_opt += data.size();
        // Prevent optimization
        asm volatile("" : : "g"(data.data()) : "memory");
    }
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed_optimized = end - start;
    std::cout << "Optimized: " << elapsed_optimized.count() << " ms" << std::endl;

    // Benchmark Optimized 2
    start = std::chrono::high_resolution_clock::now();
    size_t total_size_opt_2 = 0;
    for (int i = 0; i < iterations; ++i) {
        auto data = serialize_message_optimized_2(message);
        total_size_opt_2 += data.size();
        // Prevent optimization
        asm volatile("" : : "g"(data.data()) : "memory");
    }
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed_optimized_2 = end - start;
    std::cout << "Optimized 2 (memcpy): " << elapsed_optimized_2.count() << " ms" << std::endl;

    std::cout << "Improvement 1: " << (elapsed_original.count() / elapsed_optimized.count()) << "x" << std::endl;
    std::cout << "Improvement 2: " << (elapsed_original.count() / elapsed_optimized_2.count()) << "x" << std::endl;

    if (total_size != total_size_opt || total_size != total_size_opt_2) {
        std::cerr << "Error: Output sizes differ!" << std::endl;
        return 1;
    }

    return 0;
}
