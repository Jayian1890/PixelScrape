#include "bencode_parser.hpp"
#include <stdexcept>
#include <sstream>
#include <algorithm>

namespace pixelscrape {

BencodeParser::BencodeParser(const std::string& data) : data_(data), pos_(0) {}

BencodeValue BencodeParser::parse(const std::string& data) {
    size_t consumed;
    return parse(data, consumed);
}

BencodeValue BencodeParser::parse(const std::string& data, size_t& consumed) {
    BencodeParser parser(data);
    BencodeValue value = parser.parse_value();
    consumed = parser.pos_;
    return value;
}

BencodeValue BencodeParser::parse_value() {
    if (pos_ >= data_.size()) {
        throw std::runtime_error("Unexpected end of data");
    }

    char c = data_[pos_];
    switch (c) {
        case 'i':
            return parse_integer();
        case 'l':
            return parse_list();
        case 'd':
            return parse_dict();
        default:
            if (std::isdigit(c)) {
                return parse_string();
            }
            throw std::runtime_error("Invalid bencode data at position " + std::to_string(pos_));
    }
}

BencodeInteger BencodeParser::parse_integer() {
    if (data_[pos_] != 'i') {
        throw std::runtime_error("Expected 'i' for integer");
    }
    pos_++; // skip 'i'

    size_t end_pos = data_.find('e', pos_);
    if (end_pos == std::string::npos) {
        throw std::runtime_error("Unterminated integer");
    }

    std::string int_str = data_.substr(pos_, end_pos - pos_);
    pos_ = end_pos + 1;

    // Check for leading zeros (except for zero itself)
    if (int_str.size() > 1 && int_str[0] == '0') {
        throw std::runtime_error("Leading zeros not allowed in integers");
    }
    if (int_str.size() > 1 && int_str[0] == '-' && int_str[1] == '0') {
        throw std::runtime_error("Negative zero not allowed");
    }

    return std::stoll(int_str);
}

BencodeString BencodeParser::parse_string() {
    size_t colon_pos = data_.find(':', pos_);
    if (colon_pos == std::string::npos) {
        throw std::runtime_error("Unterminated string length");
    }

    std::string length_str = data_.substr(pos_, colon_pos - pos_);
    size_t length = std::stoul(length_str);
    pos_ = colon_pos + 1;

    if (pos_ + length > data_.size()) {
        throw std::runtime_error("String length exceeds data size");
    }

    std::string result = data_.substr(pos_, length);
    pos_ += length;
    return result;
}

std::unique_ptr<BencodeList> BencodeParser::parse_list() {
    if (data_[pos_] != 'l') {
        throw std::runtime_error("Expected 'l' for list");
    }
    pos_++; // skip 'l'

    auto result = std::make_unique<BencodeList>();
    while (pos_ < data_.size() && data_[pos_] != 'e') {
        BencodeValue value = parse_value();
        result->items.push_back(std::move(value));
    }

    if (pos_ >= data_.size() || data_[pos_] != 'e') {
        throw std::runtime_error("Unterminated list");
    }
    pos_++; // skip 'e'
    return result;
}

std::unique_ptr<BencodeDict> BencodeParser::parse_dict() {
    if (data_[pos_] != 'd') {
        throw std::runtime_error("Expected 'd' for dictionary");
    }
    pos_++; // skip 'd'

    auto result = std::make_unique<BencodeDict>();
    while (pos_ < data_.size() && data_[pos_] != 'e') {
        BencodeString key = parse_string();
        BencodeValue value = parse_value();
        result->values[key] = std::move(value);
    }

    if (pos_ >= data_.size() || data_[pos_] != 'e') {
        throw std::runtime_error("Unterminated dictionary");
    }
    pos_++; // skip 'e'
    return result;
}

std::string BencodeParser::encode(const BencodeValue& value) {
    std::stringstream ss;

    std::visit([&ss](const auto& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, BencodeInteger>) {
            ss << "i" << val << "e";
        } else if constexpr (std::is_same_v<T, BencodeString>) {
            ss << val.size() << ":" << val;
        } else if constexpr (std::is_same_v<T, std::unique_ptr<BencodeList>>) {
            ss << "l";
            for (const auto& item : val->items) {
                ss << encode(item);
            }
            ss << "e";
        } else if constexpr (std::is_same_v<T, std::unique_ptr<BencodeDict>>) {
            ss << "d";
            // Sort keys lexicographically for deterministic encoding
            std::vector<std::string> keys;
            for (const auto& pair : val->values) {
                keys.push_back(pair.first);
            }
            std::sort(keys.begin(), keys.end());

            for (const auto& key : keys) {
                ss << key.size() << ":" << key;
                ss << encode(val->values.at(key));
            }
            ss << "e";
        }
    }, value);

    return ss.str();
}

} // namespace pixelscrape