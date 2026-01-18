/*
 * pixelLib
 * Copyright (c) 2025 Interlaced Pixel
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PIXELLIB_CORE_JSON_HPP
#define PIXELLIB_CORE_JSON_HPP

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <ios>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pixellib::core::json
{

class JSON;

struct JsonError
{
  size_t position{0};
  std::string message;
};

struct StringifyOptions
{
  bool pretty{false};
  int indent{2};
  bool escape_solidus{false};
};

class JSON
{
public:
  enum class Type
  {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
  };

  struct Number
  {
    std::string repr;

    double to_double(double fallback = 0.0) const
    {
      char *end = nullptr;
      const double v = std::strtod(repr.c_str(), &end);
      return (end && *end == '\0') ? v : fallback;
    }

    int64_t to_int64(int64_t fallback = 0) const
    {
      char *end = nullptr;
      const long long v = std::strtoll(repr.c_str(), &end, 10);
      if (!end || *end != '\0')
      {
        return fallback;
      }
      if (v < std::numeric_limits<int64_t>::min())
      {
        return fallback;
      }
      if (v > std::numeric_limits<int64_t>::max())
      {
        return fallback;
      }
      return static_cast<int64_t>(v);
    }

    bool is_integral() const
    {
      char *end = nullptr;
      (void)std::strtoll(repr.c_str(), &end, 10);
      return end && *end == '\0';
    }
  };

  using array_t = std::vector<JSON>;
  using object_t = std::vector<std::pair<std::string, JSON>>;

  JSON() : type_(Type::Null), data_(std::monostate{}) {}
  explicit JSON(std::nullptr_t) : type_(Type::Null), data_(std::monostate{}) {}
  explicit JSON(bool value) : type_(Type::Bool), data_(value) {}
  explicit JSON(Number number) : type_(Type::Number), data_(std::move(number)) {}
  explicit JSON(double number) : type_(Type::Number), data_(Number{format_number(number)}) {}
  explicit JSON(std::string value) : type_(Type::String), data_(std::move(value)) {}
  static JSON array(array_t values)
  {
    return JSON(Type::Array, std::move(values));
  }
  static JSON object(object_t values)
  {
    return JSON(Type::Object, std::move(values));
  }
  static JSON number(std::string_view repr)
  {
    return JSON(Type::Number, Number{std::string(repr)});
  }

  Type type() const
  {
    return type_;
  }

  bool is_null() const
  {
    return type_ == Type::Null;
  }
  bool is_bool() const
  {
    return type_ == Type::Bool;
  }
  bool is_number() const
  {
    return type_ == Type::Number;
  }
  bool is_string() const
  {
    return type_ == Type::String;
  }
  bool is_array() const
  {
    return type_ == Type::Array;
  }
  bool is_object() const
  {
    return type_ == Type::Object;
  }

  bool as_bool(bool fallback = false) const
  {
    if (type_ != Type::Bool)
    {
      return fallback;
    }
    return std::get<bool>(data_);
  }

  const Number &as_number() const
  {
    if (type_ != Type::Number)
    {
      throw std::logic_error("Not a number");
    }
    return std::get<Number>(data_);
  }

  const std::string &as_string() const
  {
    if (type_ != Type::String)
    {
      throw std::logic_error("Not a string");
    }
    return std::get<std::string>(data_);
  }

  const array_t &as_array() const
  {
    if (type_ != Type::Array)
    {
      throw std::logic_error("Not an array");
    }
    return std::get<array_t>(data_);
  }

  array_t &as_array()
  {
    if (type_ != Type::Array)
    {
      throw std::logic_error("Not an array");
    }
    return std::get<array_t>(data_);
  }

  const object_t &as_object() const
  {
    if (type_ != Type::Object)
    {
      throw std::logic_error("Not an object");
    }
    return std::get<object_t>(data_);
  }

  object_t &as_object()
  {
    if (type_ != Type::Object)
    {
      throw std::logic_error("Not an object");
    }
    return std::get<object_t>(data_);
  }

  JSON &push_back(JSON value)
  {
    as_array().push_back(std::move(value));
    return as_array().back();
  }

  JSON &operator[](std::string_view key)
  {
    auto &obj = as_object();
    for (auto &entry : obj)
    {
      if (entry.first == key)
      {
        return entry.second;
      }
    }
    obj.emplace_back(std::string(key), JSON());
    return obj.back().second;
  }

  const JSON *find(std::string_view key) const
  {
    if (!is_object())
    {
      return nullptr;
    }
    for (const auto &entry : std::get<object_t>(data_))
    {
      if (entry.first == key)
      {
        return &entry.second;
      }
    }
    return nullptr;
  }

  static bool parse(const std::string &text, JSON &out, JsonError *error = nullptr);
  static JSON parse_or_throw(const std::string &text);
  static bool validate(const std::string &text);

  std::string stringify(const StringifyOptions &options = {}) const;

private:
  Type type_;
  std::variant<std::monostate, bool, Number, std::string, array_t, object_t> data_;

  JSON(Type type, std::variant<std::monostate, bool, Number, std::string, array_t, object_t> data) : type_(type), data_(std::move(data)) {}

  static std::string format_number(double value)
  {
    std::ostringstream oss;
    oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
    oss << std::setprecision(15) << value;
    return oss.str();
  }

  static std::string escape_string(const std::string &input, bool escape_solidus);
  static void stringify_impl(const JSON &node, const StringifyOptions &options, std::string &out, int depth);
};

class Parser
{
public:
  explicit Parser(const std::string &text) : input(text) {}

  bool parse(JSON &out)
  {
    skip_ws();
    if (!parse_value(out))
    {
      return false;
    }
    skip_ws();
    if (pos != input.size())
    {
      return fail("Trailing characters after JSON value");
    }
    return true;
  }

  JsonError error;

private:
  const std::string &input;
  size_t pos{0};

  bool fail(std::string message)
  {
    error = {pos, std::move(message)};
    return false;
  }

  char peek() const
  {
    return pos < input.size() ? input[pos] : '\0';
  }

  bool consume(char expected)
  {
    if (peek() != expected)
    {
      return false;
    }
    ++pos;
    return true;
  }

  void skip_ws()
  {
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
    {
      ++pos;
    }
  }

  bool parse_value(JSON &out)
  {
    const char c = peek();
    switch (c)
    {
    case 'n':
      return parse_literal("null", JSON(std::nullptr_t{}), out);
    case 't':
      return parse_literal("true", JSON(true), out);
    case 'f':
      return parse_literal("false", JSON(false), out);
    case '"':
    {
      std::string value;
      if (!parse_string(value))
      {
        return false;
      }
      out = JSON(std::move(value));
      return true;
    }
    case '{':
      return parse_object(out);
    case '[':
      return parse_array(out);
    default:
      if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
      {
        return parse_number(out);
      }
      return fail("Unexpected character while parsing value");
    }
  }

  bool parse_literal(std::string_view literal, JSON value, JSON &out)
  {
    if (input.compare(pos, literal.size(), literal) != 0)
    {
      return fail("Invalid literal");
    }
    pos += literal.size();
    out = std::move(value);
    return true;
  }

  static int hex_to_int(char c)
  {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
      return 10 + (c - 'A');
    return -1;
  }

  static bool append_codepoint(uint32_t codepoint, std::string &out)
  {
    if (codepoint <= 0x7F)
    {
      out.push_back(static_cast<char>(codepoint));
      return true;
    }
    if (codepoint <= 0x7FF)
    {
      out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
      return true;
    }
    if (codepoint <= 0xFFFF)
    {
      out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
      return true;
    }
    if (codepoint <= 0x10FFFF)
    {
      out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
      return true;
    }
    return false;
  }

  bool parse_unicode_escape(std::string &out)
  {
    if (pos + 4 > input.size())
    {
      return fail("Incomplete unicode escape");
    }

    uint32_t code_unit = 0;
    for (int i = 0; i < 4; ++i)
    {
      const int v = hex_to_int(input[pos + i]);
      if (v < 0)
      {
        return fail("Invalid hex in unicode escape");
      }
      code_unit = (code_unit << 4) | static_cast<uint32_t>(v);
    }
    pos += 4;

    if (code_unit >= 0xD800 && code_unit <= 0xDBFF)
    {
      if (pos + 2 > input.size() || input[pos] != '\\' || input[pos + 1] != 'u')
      {
        return fail("Missing low surrogate for unicode escape");
      }
      pos += 2;
      if (pos + 4 > input.size())
      {
        return fail("Incomplete unicode escape");
      }
      uint32_t low = 0;
      for (int i = 0; i < 4; ++i)
      {
        const int v = hex_to_int(input[pos + i]);
        if (v < 0)
        {
          return fail("Invalid hex in unicode escape");
        }
        low = (low << 4) | static_cast<uint32_t>(v);
      }
      pos += 4;
      if (low < 0xDC00 || low > 0xDFFF)
      {
        return fail("Invalid low surrogate in unicode escape");
      }
      const uint32_t codepoint = 0x10000 + (((code_unit - 0xD800) << 10) | (low - 0xDC00));
      return append_codepoint(codepoint, out) ? true : fail("Invalid unicode codepoint");
    }

    return append_codepoint(code_unit, out) ? true : fail("Invalid unicode codepoint");
  }

  bool parse_string(std::string &out)
  {
    if (!consume('"'))
    {
      return fail("Expected opening quote for string");
    }

    while (pos < input.size())
    {
      const char c = input[pos++];
      if (c == '"')
      {
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20)
      {
        return fail("Control character in string");
      }
      if (c == '\\')
      {
        if (pos >= input.size())
        {
          return fail("Unterminated escape sequence");
        }
        const char esc = input[pos++];
        switch (esc)
        {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u':
          if (!parse_unicode_escape(out))
          {
            return false;
          }
          break;
        default:
          return fail("Invalid escape sequence in string");
        }
      }
      else
      {
        out.push_back(c);
      }
    }

    return fail("Unterminated string literal");
  }

  bool parse_number(JSON &out)
  {
    const size_t start = pos;
    if (peek() == '-')
    {
      ++pos;
    }

    if (peek() == '0')
    {
      ++pos;
    }
    else if (std::isdigit(static_cast<unsigned char>(peek())))
    {
      while (std::isdigit(static_cast<unsigned char>(peek())))
      {
        ++pos;
      }
    }
    else
    {
      return fail("Invalid number format");
    }

    if (peek() == '.')
    {
      ++pos;
      if (!std::isdigit(static_cast<unsigned char>(peek())))
      {
        return fail("Invalid fraction in number");
      }
      while (std::isdigit(static_cast<unsigned char>(peek())))
      {
        ++pos;
      }
    }

    if (peek() == 'e' || peek() == 'E')
    {
      ++pos;
      if (peek() == '+' || peek() == '-')
      {
        ++pos;
      }
      if (!std::isdigit(static_cast<unsigned char>(peek())))
      {
        return fail("Invalid exponent in number");
      }
      while (std::isdigit(static_cast<unsigned char>(peek())))
      {
        ++pos;
      }
    }

    const std::string repr = input.substr(start, pos - start);
    out = JSON::number(repr);
    return true;
  }

  bool parse_array(JSON &out)
  {
    if (!consume('['))
    {
      return fail("Expected '[' to start array");
    }
    JSON::array_t elements;
    skip_ws();
    if (consume(']'))
    {
      out = JSON::array(std::move(elements));
      return true;
    }

    while (true)
    {
      JSON value;
      skip_ws();
      if (!parse_value(value))
      {
        return false;
      }
      elements.push_back(std::move(value));
      skip_ws();
      if (consume(']'))
      {
        out = JSON::array(std::move(elements));
        return true;
      }
      if (!consume(','))
      {
        return fail("Expected ',' between array elements");
      }
      skip_ws();
    }
  }

  bool parse_object(JSON &out)
  {
    if (!consume('{'))
    {
      return fail("Expected '{' to start object");
    }
    JSON::object_t members;
    skip_ws();
    if (consume('}'))
    {
      out = JSON::object(std::move(members));
      return true;
    }

    while (true)
    {
      skip_ws();
      std::string key;
      if (!parse_string(key))
      {
        return false;
      }
      skip_ws();
      if (!consume(':'))
      {
        return fail("Expected ':' after object key");
      }
      skip_ws();
      JSON value;
      if (!parse_value(value))
      {
        return false;
      }
      members.emplace_back(std::move(key), std::move(value));
      skip_ws();
      if (consume('}'))
      {
        out = JSON::object(std::move(members));
        return true;
      }
      if (!consume(','))
      {
        return fail("Expected ',' between object members");
      }
      skip_ws();
    }
  }
};

inline bool JSON::parse(const std::string &text, JSON &out, JsonError *error)
{
  Parser parser(text);
  const bool ok = parser.parse(out);
  if (!ok && error)
  {
    *error = parser.error;
  }
  return ok;
}

inline JSON JSON::parse_or_throw(const std::string &text)
{
  JSON value;
  JsonError err;
  if (!parse(text, value, &err))
  {
    std::ostringstream oss;
    oss << "JSON parse error at position " << err.position << ": " << err.message;
    throw std::invalid_argument(oss.str());
  }
  return value;
}

inline bool JSON::validate(const std::string &text)
{
  JSON value;
  return parse(text, value, nullptr);
}

inline std::string JSON::escape_string(const std::string &input, bool escape_solidus)
{
  std::string out;
  out.reserve(input.size() + input.size() / 4);
  for (unsigned char c : input)
  {
    switch (c)
    {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    case '/':
      out += escape_solidus ? "\\/" : "/";
      break;
    default:
      if (c < 0x20)
      {
        constexpr char hex[] = "0123456789ABCDEF";
        out += "\\u00";
        out.push_back(hex[(c >> 4) & 0xF]);
        out.push_back(hex[c & 0xF]);
      }
      else
      {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return out;
}

inline void JSON::stringify_impl(const JSON &node, const StringifyOptions &options, std::string &out, int depth)
{
  switch (node.type_)
  {
  case Type::Null:
    out += "null";
    return;
  case Type::Bool:
    out += node.as_bool() ? "true" : "false";
    return;
  case Type::Number:
    out += node.as_number().repr;
    return;
  case Type::String:
    out += '"';
    out += escape_string(node.as_string(), options.escape_solidus);
    out += '"';
    return;
  case Type::Array:
  {
    const auto &arr = node.as_array();
    out += "[";
    if (arr.empty())
    {
      out += "]";
      return;
    }
    if (options.pretty)
    {
      out += "\n";
    }
    for (size_t i = 0; i < arr.size(); ++i)
    {
      if (options.pretty)
      {
        out.append((depth + 1) * options.indent, ' ');
      }
      stringify_impl(arr[i], options, out, depth + 1);
      if (i + 1 < arr.size())
      {
        out += options.pretty ? ",\n" : ",";
      }
    }
    if (options.pretty)
    {
      out += "\n";
      out.append(depth * options.indent, ' ');
    }
    out += "]";
    return;
  }
  case Type::Object:
  {
    const auto &obj = node.as_object();
    out += "{";
    if (obj.empty())
    {
      out += "}";
      return;
    }
    if (options.pretty)
    {
      out += "\n";
    }
    for (size_t i = 0; i < obj.size(); ++i)
    {
      if (options.pretty)
      {
        out.append((depth + 1) * options.indent, ' ');
      }
      out += '"';
      out += escape_string(obj[i].first, options.escape_solidus);
      out += options.pretty ? "" : "";
      out += '"';
      out += options.pretty ? ": " : ":";
      stringify_impl(obj[i].second, options, out, depth + 1);
      if (i + 1 < obj.size())
      {
        out += options.pretty ? ",\n" : ",";
      }
    }
    if (options.pretty)
    {
      out += "\n";
      out.append(depth * options.indent, ' ');
    }
    out += "}";
    return;
  }
  }
}

inline std::string JSON::stringify(const StringifyOptions &options) const
{
  std::string out;
  stringify_impl(*this, options, out, 0);
  return out;
}

} // namespace pixellib::core::json

#endif
