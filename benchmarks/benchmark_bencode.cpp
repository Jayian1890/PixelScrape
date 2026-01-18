#include "bencode_parser.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <algorithm>
#include <cassert>

using namespace pixelscrape;

std::string random_string(size_t length, std::mt19937& rng) {
    const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    std::uniform_int_distribution<> dist(0, max_index);

    std::string str(length, 0);
    for (size_t i = 0; i < length; ++i) {
        str[i] = charset[dist(rng)];
    }
    return str;
}

int main() {
    // Generate a large dictionary
    auto dict_val = std::make_unique<BencodeDict>();
    const int num_keys = 100000;
    const int key_length = 50;

    std::random_device rd;
    std::mt19937 rng(rd());

    std::cout << "Generating " << num_keys << " keys of length " << key_length << "..." << std::endl;
    for (int i = 0; i < num_keys; ++i) {
        std::string key = random_string(key_length, rng);
        dict_val->values[key] = static_cast<BencodeInteger>(i);
    }
    BencodeValue value = std::move(dict_val);

    std::cout << "Encoding..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    std::string encoded = BencodeParser::encode(value);
    auto end = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> duration = end - start;
    std::cout << "Encoding took: " << duration.count() << " ms" << std::endl;

    // Correctness check
    std::cout << "Verifying..." << std::endl;
    BencodeValue decoded = BencodeParser::parse(encoded);
    if (!std::holds_alternative<std::unique_ptr<BencodeDict>>(decoded)) {
        std::cerr << "Verification failed: Decoded value is not a dictionary" << std::endl;
        return 1;
    }

    auto& decoded_dict = std::get<std::unique_ptr<BencodeDict>>(decoded);
    if (decoded_dict->values.size() != num_keys) {
        std::cerr << "Verification failed: Size mismatch (expected " << num_keys << ", got " << decoded_dict->values.size() << ")" << std::endl;
        return 1;
    }

    // Verify sorted order in encoded string
    size_t pos = 1; // skip 'd'
    std::string last_key = "";
    for (int i = 0; i < num_keys; ++i) {
        // Parse key string: len:chars
        size_t colon_pos = encoded.find(':', pos);
        if (colon_pos == std::string::npos) {
             std::cerr << "Verification failed: Malformed encoded string at key " << i << std::endl;
             return 1;
        }
        size_t len = std::stoul(encoded.substr(pos, colon_pos - pos));
        pos = colon_pos + 1;
        std::string key = encoded.substr(pos, len);
        pos += len;

        if (key < last_key) {
             std::cerr << "Verification failed: Keys are not sorted! (" << last_key << " > " << key << ")" << std::endl;
             return 1;
        }
        last_key = key;

        // Skip value (integer)
        // i<number>e
        if (encoded[pos] != 'i') {
            std::cerr << "Verification failed: Expected integer value" << std::endl;
            return 1;
        }
        size_t e_pos = encoded.find('e', pos);
        pos = e_pos + 1;
    }

    std::cout << "Verification passed!" << std::endl;
    return 0;
}
