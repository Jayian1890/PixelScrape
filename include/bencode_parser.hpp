#pragma once

#include <variant>
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <cstdint>

namespace pixelscrape {

// Forward declarations for recursive types
struct BencodeList;
struct BencodeDict;

// Bencode value types
using BencodeInteger = int64_t;
using BencodeString = std::string;

// Bencode value variant (defined first to avoid circular dependency)
using BencodeValue = std::variant<BencodeInteger, BencodeString, std::unique_ptr<BencodeList>, std::unique_ptr<BencodeDict>>;

// Recursive list type
struct BencodeList {
    std::vector<BencodeValue> items;
};

// Recursive dict type
struct BencodeDict {
    std::unordered_map<std::string, BencodeValue> values;
};

class BencodeParser {
public:
    static BencodeValue parse(const std::string& data);
    static std::string encode(const BencodeValue& value);

private:
    BencodeParser(const std::string& data);
    BencodeValue parse_value();
    BencodeInteger parse_integer();
    BencodeString parse_string();
    std::unique_ptr<BencodeList> parse_list();
    std::unique_ptr<BencodeDict> parse_dict();

    const std::string& data_;
    size_t pos_;
};

} // namespace pixelscrape