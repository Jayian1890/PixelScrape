#include "bencode_parser.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <algorithm>
#include <cassert>

using namespace pixelscrape;

std::string random_string(size_t length) {
    auto randchar = []() -> char {
        const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
        const size_t max_index = (sizeof(charset) - 1);
        return charset[rand() % max_index];
    };
    std::string str(length, 0);
    std::generate_n(str.begin(), length, randchar);
    return str;
}

int main() {
    // Generate a large dictionary
    auto dict_val = std::make_unique<BencodeDict>();
    const int num_keys = 100000;
    const int key_length = 50;

    std::cout << "Generating " << num_keys << " keys of length " << key_length << "..." << std::endl;
    for (int i = 0; i < num_keys; ++i) {
        std::string key = random_string(key_length);
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
    // Bencode dict format: d<key1><val1><key2><val2>...e
    // keys must be sorted.
    // We can just verify that BencodeParser::encode produced something that parses back to the same data.
    // The spec requires keys to be sorted. BencodeParser::encode implementation ensures this.
    // To strictly verify sorting without trusting BencodeParser::encode, we would parse the string manually or check substrings.
    // But since we are modifying BencodeParser::encode, we should verify the output is indeed sorted.

    // Let's verify by re-encoding the decoded value and comparing strings.
    // If our sort logic is broken, the order in 'encoded' might be wrong.
    // But wait, if we use the same faulty encode, it will match.
    // We need to check if the keys in 'encoded' appear in sorted order.

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
