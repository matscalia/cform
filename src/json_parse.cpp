#include "json_parse.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace cform {
namespace {

constexpr int kMaxDepth = 512;

class Parser {
 public:
  Parser(std::string_view text, std::string& error)
      : text_(text), error_(error) {}

  std::optional<Json> parse() {
    skip_whitespace();
    Json value;
    if (!parse_value(value, 0)) return std::nullopt;
    skip_whitespace();
    if (pos_ != text_.size()) {
      fail("unexpected trailing characters");
      return std::nullopt;
    }
    return value;
  }

 private:
  bool fail(std::string message) {
    if (error_.empty()) {
      error_ = std::move(message) + " at byte " + std::to_string(pos_);
    }
    return false;
  }

  bool eof() const { return pos_ >= text_.size(); }
  char peek() const { return text_[pos_]; }
  bool digit() const {
    return !eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0;
  }

  void skip_whitespace() {
    while (!eof()) {
      const char ch = peek();
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        pos_++;
        continue;
      }
      break;
    }
  }

  bool consume(char expected) {
    if (eof() || peek() != expected) return false;
    pos_++;
    return true;
  }

  bool literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return fail("invalid literal");
    pos_ += word.size();
    return true;
  }

  bool parse_value(Json& out, int depth) {
    if (depth > kMaxDepth) return fail("maximum nesting depth exceeded");
    if (eof()) return fail("unexpected end of input");
    switch (peek()) {
      case '{':
        return parse_object(out, depth);
      case '[':
        return parse_array(out, depth);
      case '"': {
        std::string value;
        if (!parse_string(value)) return false;
        out = Json::string_value(std::move(value));
        return true;
      }
      case 't':
        if (!literal("true")) return false;
        out = Json::boolean_value(true);
        return true;
      case 'f':
        if (!literal("false")) return false;
        out = Json::boolean_value(false);
        return true;
      case 'n':
        if (!literal("null")) return false;
        out = Json::null();
        return true;
      default:
        if (peek() == '-' || digit()) return parse_number(out);
        return fail("expected a JSON value");
    }
  }

  bool parse_object(Json& out, int depth) {
    pos_++;  // '{'
    std::vector<std::pair<std::string, Json>> entries;
    skip_whitespace();
    if (consume('}')) {
      out = Json::object_value(std::move(entries));
      return true;
    }
    for (;;) {
      skip_whitespace();
      if (eof()) return fail("unterminated object");
      if (peek() != '"') return fail("expected an object key");
      std::string key;
      if (!parse_string(key)) return false;
      skip_whitespace();
      if (!consume(':')) return fail("expected ':' after an object key");
      skip_whitespace();
      Json value;
      if (!parse_value(value, depth + 1)) return false;
      entries.emplace_back(std::move(key), std::move(value));
      skip_whitespace();
      if (consume('}')) break;
      if (!consume(',')) return fail("expected ',' or '}' in an object");
    }
    out = Json::object_value(std::move(entries));
    return true;
  }

  bool parse_array(Json& out, int depth) {
    pos_++;  // '['
    std::vector<Json> items;
    skip_whitespace();
    if (consume(']')) {
      out = Json::array_value(std::move(items));
      return true;
    }
    for (;;) {
      skip_whitespace();
      Json value;
      if (!parse_value(value, depth + 1)) return false;
      items.push_back(std::move(value));
      skip_whitespace();
      if (consume(']')) break;
      if (!consume(',')) return fail("expected ',' or ']' in an array");
    }
    out = Json::array_value(std::move(items));
    return true;
  }

  bool parse_hex4(std::uint32_t& value) {
    if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
    std::uint32_t result = 0;
    for (int index = 0; index < 4; index++) {
      const char ch = text_[pos_++];
      result <<= 4;
      if (ch >= '0' && ch <= '9') result |= static_cast<std::uint32_t>(ch - '0');
      else if (ch >= 'a' && ch <= 'f') result |= static_cast<std::uint32_t>(ch - 'a' + 10);
      else if (ch >= 'A' && ch <= 'F') result |= static_cast<std::uint32_t>(ch - 'A' + 10);
      else return fail("invalid \\u escape");
    }
    value = result;
    return true;
  }

  static void append_utf8(std::string& out, std::uint32_t code) {
    if (code <= 0x7F) {
      out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool parse_string(std::string& out) {
    pos_++;  // opening quote
    out.clear();
    while (!eof()) {
      const unsigned char ch = static_cast<unsigned char>(peek());
      if (ch == '"') {
        pos_++;
        return true;
      }
      if (ch < 0x20) return fail("control character in string");
      if (ch != '\\') {
        out.push_back(static_cast<char>(ch));
        pos_++;
        continue;
      }
      pos_++;
      if (eof()) return fail("unterminated escape sequence");
      const char escape = text_[pos_++];
      switch (escape) {
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
        case 'u': {
          std::uint32_t code = 0;
          if (!parse_hex4(code)) return false;
          if (code >= 0xD800 && code <= 0xDBFF) {
            const std::size_t saved = pos_;
            bool paired = false;
            if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                text_[pos_ + 1] == 'u') {
              pos_ += 2;
              std::uint32_t low = 0;
              if (!parse_hex4(low)) return false;
              if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                paired = true;
              }
            }
            if (!paired) {
              pos_ = saved;
              code = 0xFFFD;
            }
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            code = 0xFFFD;
          }
          append_utf8(out, code);
          break;
        }
        default:
          return fail("invalid escape sequence");
      }
    }
    return fail("unterminated string");
  }

  bool parse_number(Json& out) {
    const std::size_t start = pos_;
    bool is_integer = true;
    consume('-');
    if (eof()) return fail("invalid number");
    if (peek() == '0') {
      pos_++;
      if (digit()) return fail("invalid number");
    } else if (peek() >= '1' && peek() <= '9') {
      while (digit()) pos_++;
    } else {
      return fail("invalid number");
    }
    if (!eof() && peek() == '.') {
      is_integer = false;
      pos_++;
      if (!digit()) return fail("invalid number");
      while (digit()) pos_++;
    }
    if (!eof() && (peek() == 'e' || peek() == 'E')) {
      is_integer = false;
      pos_++;
      if (!eof() && (peek() == '+' || peek() == '-')) pos_++;
      if (!digit()) return fail("invalid number");
      while (digit()) pos_++;
    }
    const std::string token(text_.substr(start, pos_ - start));
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size()) return fail("invalid number");
    out = Json::number_value(value, is_integer);
    return true;
  }

  std::string_view text_;
  std::string& error_;
  std::size_t pos_ = 0;
};

}  // namespace

std::optional<Json> parse_json_fallback(std::string_view text,
                                        std::string& error) {
  Parser parser(text, error);
  return parser.parse();
}

}  // namespace cform
