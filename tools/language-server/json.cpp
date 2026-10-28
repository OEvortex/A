#include "json.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

namespace jsonlite {
namespace {

const std::string kEmpty;

void appendUtf8(std::string& out, unsigned codePoint) {
  if (codePoint <= 0x7F) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
}

void dumpString(std::string& out, const std::string& value) {
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[(c >> 4) & 0xF]);
          out.push_back(hex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void dumpValue(std::string& out, const Value& value) {
  switch (value.type()) {
    case Value::Type::Null:
      out += "null";
      break;
    case Value::Type::Bool:
      out += value.asBool() ? "true" : "false";
      break;
    case Value::Type::Number: {
      double number = value.asNumber();
      if (!std::isfinite(number)) {
        out += "null";
        return;
      }
      if (number == std::floor(number) && std::fabs(number) < 9.0e15) {
        std::ostringstream stream;
        stream.precision(0);
        stream << std::fixed << number;
        out += stream.str();
      } else {
        std::ostringstream stream;
        stream.precision(17);
        stream << number;
        out += stream.str();
      }
      break;
    }
    case Value::Type::String:
      dumpString(out, value.asString());
      break;
    case Value::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const Value& element : value.asArray()) {
        if (!first) out.push_back(',');
        first = false;
        dumpValue(out, element);
      }
      out.push_back(']');
      break;
    }
    case Value::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& member : value.asObject()) {
        if (!first) out.push_back(',');
        first = false;
        dumpString(out, member.first);
        out.push_back(':');
        dumpValue(out, member.second);
      }
      out.push_back('}');
      break;
    }
  }
}

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  Value parseDocument(std::string* error) {
    skipWhitespace();
    Value value = parseValue(0);
    if (failed_) {
      if (error) *error = error_;
      return Value();
    }
    skipWhitespace();
    if (pos_ != text_.size()) {
      fail("unexpected trailing characters");
      if (error) *error = error_;
      return Value();
    }
    return value;
  }

 private:
  void fail(const std::string& message) {
    if (failed_) return;
    failed_ = true;
    error_ = message + " at byte " + std::to_string(pos_);
  }

  bool atEnd() const { return pos_ >= text_.size(); }
  char peek() const { return atEnd() ? '\0' : text_[pos_]; }

  void skipWhitespace() {
    while (!atEnd()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool literal(const char* word) {
    std::size_t length = std::strlen(word);
    if (text_.compare(pos_, length, word) != 0) return false;
    pos_ += length;
    return true;
  }

  Value parseValue(int depth) {
    if (depth > 512) {
      fail("document is nested too deeply");
      return Value();
    }
    if (atEnd()) {
      fail("unexpected end of input");
      return Value();
    }
    char c = peek();
    if (c == '{') return parseObject(depth + 1);
    if (c == '[') return parseArray(depth + 1);
    if (c == '"') return Value(parseString());
    if (c == 't') {
      if (literal("true")) return Value(true);
      fail("invalid literal");
      return Value();
    }
    if (c == 'f') {
      if (literal("false")) return Value(false);
      fail("invalid literal");
      return Value();
    }
    if (c == 'n') {
      if (literal("null")) return Value();
      fail("invalid literal");
      return Value();
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
    fail("unexpected character");
    return Value();
  }

  Value parseObject(int depth) {
    Value::Object object;
    ++pos_;  // '{'
    skipWhitespace();
    if (!atEnd() && peek() == '}') {
      ++pos_;
      return Value(std::move(object));
    }
    while (true) {
      skipWhitespace();
      if (atEnd() || peek() != '"') {
        fail("expected an object key");
        return Value();
      }
      std::string key = parseString();
      skipWhitespace();
      if (atEnd() || peek() != ':') {
        fail("expected ':' after an object key");
        return Value();
      }
      ++pos_;
      skipWhitespace();
      Value value = parseValue(depth);
      if (failed_) return Value();
      object.emplace_back(std::move(key), std::move(value));
      skipWhitespace();
      if (atEnd()) {
        fail("unterminated object");
        return Value();
      }
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        break;
      }
      fail("expected ',' or '}' in object");
      return Value();
    }
    return Value(std::move(object));
  }

  Value parseArray(int depth) {
    Value::Array array;
    ++pos_;  // '['
    skipWhitespace();
    if (!atEnd() && peek() == ']') {
      ++pos_;
      return Value(std::move(array));
    }
    while (true) {
      skipWhitespace();
      Value value = parseValue(depth);
      if (failed_) return Value();
      array.push_back(std::move(value));
      skipWhitespace();
      if (atEnd()) {
        fail("unterminated array");
        return Value();
      }
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        break;
      }
      fail("expected ',' or ']' in array");
      return Value();
    }
    return Value(std::move(array));
  }

  unsigned parseHex4() {
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      if (atEnd()) {
        fail("truncated \\u escape");
        return 0;
      }
      char c = text_[pos_++];
      unsigned digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<unsigned>(c - 'A' + 10);
      } else {
        fail("invalid hex digit in \\u escape");
        return 0;
      }
      value = value * 16 + digit;
    }
    return value;
  }

  std::string parseString() {
    std::string out;
    ++pos_;  // opening quote
    while (!atEnd()) {
      unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      if (c == '"') return out;
      if (c < 0x20) {
        fail("unescaped control character in string");
        return out;
      }
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (atEnd()) {
        fail("unterminated escape");
        return out;
      }
      char esc = text_[pos_++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned codePoint = parseHex4();
          if (failed_) return out;
          if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
              pos_ += 2;
              unsigned low = parseHex4();
              if (failed_) return out;
              if (low >= 0xDC00 && low <= 0xDFFF) {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
              } else {
                appendUtf8(out, codePoint);
                codePoint = low;
              }
            }
          }
          appendUtf8(out, codePoint);
          break;
        }
        default:
          fail("invalid escape sequence");
          return out;
      }
    }
    fail("unterminated string");
    return out;
  }

  Value parseNumber() {
    std::size_t start = pos_;
    if (peek() == '-') ++pos_;
    if (atEnd() || peek() < '0' || peek() > '9') {
      fail("invalid number");
      return Value();
    }
    if (peek() == '0') {
      ++pos_;
    } else {
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    if (!atEnd() && peek() == '.') {
      ++pos_;
      if (atEnd() || peek() < '0' || peek() > '9') {
        fail("invalid number fraction");
        return Value();
      }
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
      ++pos_;
      if (!atEnd() && (peek() == '+' || peek() == '-')) ++pos_;
      if (atEnd() || peek() < '0' || peek() > '9') {
        fail("invalid number exponent");
        return Value();
      }
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    std::string token = text_.substr(start, pos_ - start);
    char* end = nullptr;
    double number = std::strtod(token.c_str(), &end);
    if (end == token.c_str() || *end != '\0' || !std::isfinite(number)) {
      fail("invalid number");
      return Value();
    }
    return Value(number);
  }

  const std::string& text_;
  std::size_t pos_ = 0;
  bool failed_ = false;
  std::string error_;
};

}  // namespace

Value::Value() = default;
Value::Value(std::nullptr_t) : type_(Type::Null) {}
Value::Value(bool value) : type_(Type::Bool), bool_(value) {}
Value::Value(int value) : type_(Type::Number), number_(value) {}
Value::Value(long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
Value::Value(double value) : type_(Type::Number), number_(value) {}
Value::Value(const char* value) : type_(Type::String), string_(value ? value : "") {}
Value::Value(std::string value) : type_(Type::String), string_(std::move(value)) {}
Value::Value(Array value) : type_(Type::Array), array_(std::move(value)) {}
Value::Value(Object value) : type_(Type::Object), object_(std::move(value)) {}

bool Value::asBool(bool fallback) const { return type_ == Type::Bool ? bool_ : fallback; }
double Value::asNumber(double fallback) const { return type_ == Type::Number ? number_ : fallback; }
long long Value::asInt(long long fallback) const {
  return type_ == Type::Number ? static_cast<long long>(number_) : fallback;
}
const std::string& Value::asString() const { return type_ == Type::String ? string_ : kEmpty; }
const Value::Array& Value::asArray() const { return array_; }
const Value::Object& Value::asObject() const { return object_; }
Value::Array& Value::asArray() { return array_; }
Value::Object& Value::asObject() { return object_; }

Value& Value::operator[](const std::string& key) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  for (auto& member : object_) {
    if (member.first == key) return member.second;
  }
  object_.emplace_back(key, Value());
  return object_.back().second;
}

const Value* Value::find(const std::string& key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& member : object_) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

void Value::push(Value value) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    array_.clear();
  }
  array_.push_back(std::move(value));
}

void Value::set(std::string key, Value value) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  for (auto& member : object_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

std::size_t Value::size() const {
  if (type_ == Type::Array) return array_.size();
  if (type_ == Type::Object) return object_.size();
  return 0;
}

std::string Value::dump() const {
  std::string out;
  dumpValue(out, *this);
  return out;
}

Value parse(const std::string& text, std::string* error) {
  Parser parser(text);
  return parser.parseDocument(error);
}

}  // namespace jsonlite
