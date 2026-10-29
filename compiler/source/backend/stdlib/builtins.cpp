// Standard-library builtins: json, binary, testing, log, metrics, trace, net,
// and http. They live in the interpreter because they need dynamic values,
// closures (property tests, HTTP handlers), and platform sockets.

#include "backend/interpreter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <system_error>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef A_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#include "strings/format.hpp"
#include "strings/utf8.hpp"

namespace a {
namespace {

// ------------------------------------------------------------- value makers

Value vVoid() { return Value{}; }

Value vBool(bool value) {
  Value out;
  out.data = value;
  return out;
}

Value vInt(std::int64_t value) {
  Value out;
  out.data = value;
  return out;
}

Value vFloat(double value) {
  Value out;
  out.data = value;
  return out;
}

Value vString(std::string value) {
  Value out;
  out.data = std::move(value);
  return out;
}

Value vArray(std::vector<Value> items) {
  auto array = std::make_shared<std::vector<Value>>(std::move(items));
  Value out;
  out.data = std::move(array);
  return out;
}

Value vMap(std::vector<std::pair<Value, Value>> entries) {
  auto map = std::make_shared<MapObj>();
  map->entries = std::move(entries);
  Value out;
  out.data = std::move(map);
  return out;
}

Value vMapFromStrings(const std::vector<std::pair<std::string, std::string>>& entries) {
  std::vector<std::pair<Value, Value>> converted;
  converted.reserve(entries.size());
  for (const auto& entry : entries) {
    converted.emplace_back(vString(entry.first), vString(entry.second));
  }
  return vMap(std::move(converted));
}

Value vOptional(bool hasValue, Value value) {
  auto optional = std::make_shared<OptionalValue>();
  optional->hasValue = hasValue;
  optional->value = std::move(value);
  Value out;
  out.data = std::move(optional);
  return out;
}

Value vNone() { return vOptional(false, Value{}); }

Value vSome(Value value) { return vOptional(true, std::move(value)); }

Value vTuple(std::vector<Value> items) {
  auto tuple = std::make_shared<TupleValue>();
  tuple->items = std::move(items);
  Value out;
  out.data = std::move(tuple);
  return out;
}

std::string displayBuiltinName(const std::string& name) {
  static const char* modules[] = {"json",   "binary", "testing", "log",
                                  "metrics", "trace", "net",     "http"};
  for (const char* module : modules) {
    const std::string prefix = std::string(module) + "_";
    if (name.rfind(prefix, 0) == 0) return std::string(module) + "." + name.substr(prefix.size());
  }
  return name;
}

// ------------------------------------------------------------------- JSON

struct JsonFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void appendUtf8(std::string& out, char32_t codepoint) { out += utf8Encode(codepoint); }

class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : text_(text) {}

  Value parse() {
    skipSpace();
    Value value = parseValue();
    skipSpace();
    if (pos_ != text_.size()) {
      throwFailure("unexpected trailing characters");
    }
    return value;
  }

 private:
  [[noreturn]] void throwFailure(const std::string& message) {
    throw JsonFailure(message + " at offset " + std::to_string(pos_));
  }

  bool atEnd() const { return pos_ >= text_.size(); }
  char peek() const { return atEnd() ? '\0' : text_[pos_]; }

  void skipSpace() {
    while (!atEnd()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  void expectLiteral(std::string_view literal) {
    if (text_.compare(pos_, literal.size(), literal) != 0) throwFailure("invalid literal");
    pos_ += literal.size();
  }

  Value parseValue() {
    if (atEnd()) throwFailure("unexpected end of input");
    switch (peek()) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': return vString(parseString());
      case 't': expectLiteral("true"); return vBool(true);
      case 'f': expectLiteral("false"); return vBool(false);
      case 'n': expectLiteral("null"); return Value{};
      default:
        if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return parseNumber();
        throwFailure("unexpected character");
    }
  }

  Value parseObject() {
    ++pos_;  // '{'
    auto map = std::make_shared<MapObj>();
    skipSpace();
    if (peek() == '}') {
      ++pos_;
      return vMap(std::move(map->entries));
    }
    while (true) {
      skipSpace();
      if (peek() != '"') throwFailure("expected a string key");
      std::string key = parseString();
      skipSpace();
      if (peek() != ':') throwFailure("expected ':'");
      ++pos_;
      skipSpace();
      Value value = parseValue();
      bool replaced = false;
      for (auto& entry : map->entries) {
        auto* existing = std::get_if<std::string>(&entry.first.data);
        if (existing && *existing == key) {
          entry.second = std::move(value);
          replaced = true;
          break;
        }
      }
      if (!replaced) map->entries.emplace_back(vString(std::move(key)), std::move(value));
      skipSpace();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        break;
      }
      throwFailure("expected ',' or '}'");
    }
    std::vector<std::pair<Value, Value>> entries = std::move(map->entries);
    return vMap(std::move(entries));
  }

  Value parseArray() {
    ++pos_;  // '['
    std::vector<Value> items;
    skipSpace();
    if (peek() == ']') {
      ++pos_;
      return vArray(std::move(items));
    }
    while (true) {
      skipSpace();
      items.push_back(parseValue());
      skipSpace();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        break;
      }
      throwFailure("expected ',' or ']'");
    }
    return vArray(std::move(items));
  }

  static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  char32_t parseHex4() {
    char32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      if (atEnd()) throwFailure("truncated \\u escape");
      int digit = hexDigit(text_[pos_++]);
      if (digit < 0) throwFailure("invalid \\u escape");
      value = value * 16 + static_cast<char32_t>(digit);
    }
    return value;
  }

  std::string parseString() {
    ++pos_;  // opening quote
    std::string out;
    while (true) {
      if (atEnd()) throwFailure("unterminated string");
      unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      if (c == '"') break;
      if (c < 0x20) throwFailure("raw control character in string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (atEnd()) throwFailure("unterminated escape");
      char escape = text_[pos_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          char32_t codepoint = parseHex4();
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (!atEnd() && text_[pos_] == '\\' && pos_ + 1 < text_.size() &&
                text_[pos_ + 1] == 'u') {
              pos_ += 2;
              char32_t low = parseHex4();
              if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
              } else {
                codepoint = 0xFFFD;
              }
            } else {
              codepoint = 0xFFFD;
            }
          } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            codepoint = 0xFFFD;
          }
          appendUtf8(out, codepoint);
          break;
        }
        default: throwFailure("invalid escape sequence");
      }
    }
    return out;
  }

  Value parseNumber() {
    const std::size_t start = pos_;
    if (peek() == '-') ++pos_;
    if (atEnd()) throwFailure("invalid number");
    if (peek() == '0') {
      ++pos_;
      if (!atEnd() && peek() >= '0' && peek() <= '9') throwFailure("leading zero in number");
    } else if (peek() >= '1' && peek() <= '9') {
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    } else {
      throwFailure("invalid number");
    }
    bool isFloat = false;
    if (!atEnd() && peek() == '.') {
      isFloat = true;
      ++pos_;
      if (atEnd() || peek() < '0' || peek() > '9') throwFailure("expected digits after '.'");
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
      isFloat = true;
      ++pos_;
      if (!atEnd() && (peek() == '+' || peek() == '-')) ++pos_;
      if (atEnd() || peek() < '0' || peek() > '9') throwFailure("expected exponent digits");
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    const std::string token = text_.substr(start, pos_ - start);
    if (!isFloat) {
      std::int64_t value = 0;
      auto result = std::from_chars(token.data(), token.data() + token.size(), value);
      if (result.ec == std::errc() && result.ptr == token.data() + token.size()) {
        return vInt(value);
      }
    }
    std::size_t used = 0;
    double value = 0.0;
    try {
      value = std::stod(token, &used);
    } catch (const std::exception&) {
      throwFailure("number out of range");
    }
    if (used != token.size()) throwFailure("invalid number");
    return vFloat(value);
  }

  const std::string& text_;
  std::size_t pos_ = 0;
};

std::string shortTypeName(const std::string& qualified) {
  std::size_t pos = qualified.rfind("::");
  return pos == std::string::npos ? qualified : qualified.substr(pos + 2);
}

void appendEscapedString(std::string& out, const std::string& text) {
  out.push_back('"');
  for (unsigned char c : text) {
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
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void appendJsonNumber(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  out.append(buffer, result.ptr);
}

void appendIndent(std::string& out, int depth) {
  out.push_back('\n');
  out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void writeJsonValue(std::string& out, const Value& value, bool pretty, int depth) {
  if (std::holds_alternative<std::monostate>(value.data)) {
    out += "null";
    return;
  }
  if (auto* boolean = std::get_if<bool>(&value.data)) {
    out += *boolean ? "true" : "false";
    return;
  }
  if (auto* number = std::get_if<std::int64_t>(&value.data)) {
    out += std::to_string(*number);
    return;
  }
  if (auto* number = std::get_if<double>(&value.data)) {
    appendJsonNumber(out, *number);
    return;
  }
  if (auto* character = std::get_if<char32_t>(&value.data)) {
    appendEscapedString(out, utf8Encode(*character));
    return;
  }
  if (auto* text = std::get_if<std::string>(&value.data)) {
    appendEscapedString(out, *text);
    return;
  }
  if (auto* array = std::get_if<ArrayPtr>(&value.data)) {
    if ((*array)->empty()) {
      out += "[]";
      return;
    }
    out.push_back('[');
    for (std::size_t i = 0; i < (*array)->size(); ++i) {
      if (i) out.push_back(',');
      if (pretty) appendIndent(out, depth + 1);
      writeJsonValue(out, (**array)[i], pretty, depth + 1);
    }
    if (pretty) appendIndent(out, depth);
    out.push_back(']');
    return;
  }
  if (auto* view = std::get_if<ArrayViewPtr>(&value.data)) {
    const auto& storage = *(*view)->storage;
    if ((*view)->length == 0) {
      out += "[]";
      return;
    }
    out.push_back('[');
    for (std::size_t i = 0; i < (*view)->length; ++i) {
      if (i) out.push_back(',');
      if (pretty) appendIndent(out, depth + 1);
      writeJsonValue(out, storage[(*view)->offset + i], pretty, depth + 1);
    }
    if (pretty) appendIndent(out, depth);
    out.push_back(']');
    return;
  }
  if (auto* map = std::get_if<MapPtr>(&value.data)) {
    if ((*map)->entries.empty()) {
      out += "{}";
      return;
    }
    out.push_back('{');
    bool first = true;
    for (const auto& entry : (*map)->entries) {
      if (!first) out.push_back(',');
      first = false;
      if (pretty) appendIndent(out, depth + 1);
      const auto* key = std::get_if<std::string>(&entry.first.data);
      if (!key) throw JsonFailure("JSON object keys must be strings");
      appendEscapedString(out, *key);
      out.push_back(':');
      if (pretty) out.push_back(' ');
      writeJsonValue(out, entry.second, pretty, depth + 1);
    }
    if (pretty) appendIndent(out, depth);
    out.push_back('}');
    return;
  }
  if (auto* set = std::get_if<SetPtr>(&value.data)) {
    std::vector<Value> items = (*set)->elements;
    Value asArray = vArray(std::move(items));
    writeJsonValue(out, asArray, pretty, depth);
    return;
  }
  if (auto* object = std::get_if<StructPtr>(&value.data)) {
    std::vector<std::pair<Value, Value>> entries;
    entries.reserve((*object)->fields.size());
    for (const auto& field : (*object)->fields) {
      entries.emplace_back(vString(field.first), field.second);
    }
    Value asMap = vMap(std::move(entries));
    writeJsonValue(out, asMap, pretty, depth);
    return;
  }
  if (auto* optional = std::get_if<OptionalPtr>(&value.data)) {
    if (!(*optional)->hasValue) {
      out += "null";
      return;
    }
    writeJsonValue(out, (*optional)->value, pretty, depth);
    return;
  }
  if (auto* result = std::get_if<ResultPtr>(&value.data)) {
    std::vector<std::pair<Value, Value>> entries;
    entries.emplace_back(vString("ok"), vBool((*result)->ok));
    entries.emplace_back(vString("value"), (*result)->value);
    Value asMap = vMap(std::move(entries));
    writeJsonValue(out, asMap, pretty, depth);
    return;
  }
  if (auto* tuple = std::get_if<TuplePtr>(&value.data)) {
    Value asArray = vArray((*tuple)->items);
    writeJsonValue(out, asArray, pretty, depth);
    return;
  }
  if (auto* range = std::get_if<RangeValue>(&value.data)) {
    std::vector<std::pair<Value, Value>> entries;
    entries.emplace_back(vString("start"), vInt(range->start));
    entries.emplace_back(vString("end"), vInt(range->end));
    entries.emplace_back(vString("step"), vInt(range->step));
    Value asMap = vMap(std::move(entries));
    writeJsonValue(out, asMap, pretty, depth);
    return;
  }
  throw JsonFailure("this value cannot be serialized as JSON");
}

std::string writeJson(const Value& value, bool pretty) {
  std::string out;
  writeJsonValue(out, value, pretty, 0);
  return out;
}

bool isJsonContainer(const Value& value) {
  return std::holds_alternative<MapPtr>(value.data) ||
         std::holds_alternative<ArrayPtr>(value.data) ||
         std::holds_alternative<StructPtr>(value.data);
}

std::string jsonTypeName(const Value& value) {
  if (std::holds_alternative<std::monostate>(value.data)) return "null";
  if (std::holds_alternative<bool>(value.data)) return "bool";
  if (std::holds_alternative<std::int64_t>(value.data) ||
      std::holds_alternative<double>(value.data)) {
    return "number";
  }
  if (std::holds_alternative<std::string>(value.data) ||
      std::holds_alternative<char32_t>(value.data)) {
    return "string";
  }
  if (std::holds_alternative<ArrayPtr>(value.data) ||
      std::holds_alternative<ArrayViewPtr>(value.data) ||
      std::holds_alternative<SetPtr>(value.data) ||
      std::holds_alternative<TuplePtr>(value.data)) {
    return "array";
  }
  if (std::holds_alternative<MapPtr>(value.data) ||
      std::holds_alternative<StructPtr>(value.data)) {
    return "object";
  }
  if (std::holds_alternative<OptionalPtr>(value.data)) {
    auto* optional = std::get_if<OptionalPtr>(&value.data);
    return (*optional)->hasValue ? jsonTypeName((*optional)->value) : "null";
  }
  return "unknown";
}

bool jsonMapLookup(const Value& object, const std::string& key, Value& out) {
  if (auto* map = std::get_if<MapPtr>(&object.data)) {
    for (const auto& entry : (*map)->entries) {
      auto* existing = std::get_if<std::string>(&entry.first.data);
      if (existing && *existing == key) {
        out = entry.second;
        return true;
      }
    }
    return false;
  }
  if (auto* object2 = std::get_if<StructPtr>(&object.data)) {
    for (const auto& field : (*object2)->fields) {
      if (field.first == key) {
        out = field.second;
        return true;
      }
    }
  }
  return false;
}

void jsonObjectKeys(const Value& object, std::vector<std::string>& out) {
  if (auto* map = std::get_if<MapPtr>(&object.data)) {
    for (const auto& entry : (*map)->entries) {
      const auto* key = std::get_if<std::string>(&entry.first.data);
      out.push_back(key ? *key : formatValue(entry.first, false));
    }
    return;
  }
  if (auto* object2 = std::get_if<StructPtr>(&object.data)) {
    for (const auto& field : (*object2)->fields) out.push_back(field.first);
  }
}

Value jsonToMap(const Value& object) {
  if (std::holds_alternative<MapPtr>(object.data)) return object;
  if (auto* object2 = std::get_if<StructPtr>(&object.data)) {
    std::vector<std::pair<Value, Value>> entries;
    entries.reserve((*object2)->fields.size());
    for (const auto& field : (*object2)->fields) {
      entries.emplace_back(vString(field.first), field.second);
    }
    return vMap(std::move(entries));
  }
  return object;
}

// ---------------------------------------------------------------- binary

constexpr std::uint8_t kBinNull = 0;
constexpr std::uint8_t kBinFalse = 1;
constexpr std::uint8_t kBinTrue = 2;
constexpr std::uint8_t kBinInt = 3;
constexpr std::uint8_t kBinFloat = 4;
constexpr std::uint8_t kBinString = 5;
constexpr std::uint8_t kBinArray = 6;
constexpr std::uint8_t kBinMap = 7;
constexpr std::uint8_t kBinStruct = 8;
constexpr std::uint8_t kBinSet = 9;
constexpr std::uint8_t kBinNone = 10;
constexpr std::uint8_t kBinSome = 11;
constexpr std::uint8_t kBinTuple = 12;
constexpr std::uint8_t kBinChar = 13;
constexpr std::uint8_t kBinOk = 14;
constexpr std::uint8_t kBinErr = 15;

struct BinaryFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void writeVarint(std::vector<std::uint8_t>& out, std::uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<std::uint8_t>(value) | 0x80);
    value >>= 7;
  }
  out.push_back(static_cast<std::uint8_t>(value));
}

std::uint64_t readVarint(const std::vector<std::uint8_t>& bytes, std::size_t& pos) {
  std::uint64_t value = 0;
  int shift = 0;
  while (true) {
    if (pos >= bytes.size()) throw BinaryFailure("truncated varint");
    std::uint8_t byte = bytes[pos++];
    value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) return value;
    shift += 7;
    if (shift > 63) throw BinaryFailure("varint is too long");
  }
}

void writeString(std::vector<std::uint8_t>& out, const std::string& text) {
  writeVarint(out, text.size());
  out.insert(out.end(), text.begin(), text.end());
}

std::string readString(const std::vector<std::uint8_t>& bytes, std::size_t& pos) {
  std::uint64_t length = readVarint(bytes, pos);
  if (length > bytes.size() - pos) throw BinaryFailure("truncated string");
  std::string out(reinterpret_cast<const char*>(bytes.data() + pos),
                  static_cast<std::size_t>(length));
  pos += static_cast<std::size_t>(length);
  return out;
}

void encodeBinary(std::vector<std::uint8_t>& out, const Value& value) {
  if (std::holds_alternative<std::monostate>(value.data)) {
    out.push_back(kBinNull);
    return;
  }
  if (auto* boolean = std::get_if<bool>(&value.data)) {
    out.push_back(*boolean ? kBinTrue : kBinFalse);
    return;
  }
  if (auto* number = std::get_if<std::int64_t>(&value.data)) {
    out.push_back(kBinInt);
    std::uint64_t zigzag = (static_cast<std::uint64_t>(*number) << 1) ^
                           static_cast<std::uint64_t>(*number >> 63);
    writeVarint(out, zigzag);
    return;
  }
  if (auto* number = std::get_if<double>(&value.data)) {
    out.push_back(kBinFloat);
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(*number));
    std::memcpy(&bits, number, sizeof(bits));
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    return;
  }
  if (auto* character = std::get_if<char32_t>(&value.data)) {
    out.push_back(kBinChar);
    writeVarint(out, *character);
    return;
  }
  if (auto* text = std::get_if<std::string>(&value.data)) {
    out.push_back(kBinString);
    writeString(out, *text);
    return;
  }
  if (auto* array = std::get_if<ArrayPtr>(&value.data)) {
    out.push_back(kBinArray);
    writeVarint(out, (*array)->size());
    for (const Value& item : **array) encodeBinary(out, item);
    return;
  }
  if (auto* view = std::get_if<ArrayViewPtr>(&value.data)) {
    out.push_back(kBinArray);
    writeVarint(out, (*view)->length);
    for (std::size_t i = 0; i < (*view)->length; ++i) {
      encodeBinary(out, (*(*view)->storage)[(*view)->offset + i]);
    }
    return;
  }
  if (auto* map = std::get_if<MapPtr>(&value.data)) {
    out.push_back(kBinMap);
    writeVarint(out, (*map)->entries.size());
    for (const auto& entry : (*map)->entries) {
      encodeBinary(out, entry.first);
      encodeBinary(out, entry.second);
    }
    return;
  }
  if (auto* set = std::get_if<SetPtr>(&value.data)) {
    out.push_back(kBinSet);
    writeVarint(out, (*set)->elements.size());
    for (const Value& item : (*set)->elements) encodeBinary(out, item);
    return;
  }
  if (auto* object = std::get_if<StructPtr>(&value.data)) {
    out.push_back(kBinStruct);
    writeString(out, shortTypeName((*object)->typeName));
    writeVarint(out, (*object)->fields.size());
    for (const auto& field : (*object)->fields) {
      writeString(out, field.first);
      encodeBinary(out, field.second);
    }
    return;
  }
  if (auto* optional = std::get_if<OptionalPtr>(&value.data)) {
    if (!(*optional)->hasValue) {
      out.push_back(kBinNone);
      return;
    }
    out.push_back(kBinSome);
    encodeBinary(out, (*optional)->value);
    return;
  }
  if (auto* result = std::get_if<ResultPtr>(&value.data)) {
    out.push_back((*result)->ok ? kBinOk : kBinErr);
    encodeBinary(out, (*result)->value);
    return;
  }
  if (auto* tuple = std::get_if<TuplePtr>(&value.data)) {
    out.push_back(kBinTuple);
    writeVarint(out, (*tuple)->items.size());
    for (const Value& item : (*tuple)->items) encodeBinary(out, item);
    return;
  }
  throw BinaryFailure("this value cannot be encoded");
}

Value decodeBinary(const std::vector<std::uint8_t>& bytes, std::size_t& pos) {
  if (pos >= bytes.size()) throw BinaryFailure("truncated value");
  std::uint8_t tag = bytes[pos++];
  switch (tag) {
    case kBinNull: return Value{};
    case kBinFalse: return vBool(false);
    case kBinTrue: return vBool(true);
    case kBinInt: {
      std::uint64_t zigzag = readVarint(bytes, pos);
      std::int64_t value = static_cast<std::int64_t>((zigzag >> 1) ^
                                                     (~(zigzag & 1) + 1));
      return vInt(value);
    }
    case kBinFloat: {
      if (bytes.size() - pos < 8) throw BinaryFailure("truncated float");
      std::uint64_t bits = 0;
      for (int i = 0; i < 8; ++i) bits |= static_cast<std::uint64_t>(bytes[pos++]) << (i * 8);
      double value = 0.0;
      std::memcpy(&value, &bits, sizeof(value));
      return vFloat(value);
    }
    case kBinChar: {
      std::uint64_t codepoint = readVarint(bytes, pos);
      if (codepoint > 0x10FFFF) throw BinaryFailure("invalid character");
      Value out;
      out.data = static_cast<char32_t>(codepoint);
      return out;
    }
    case kBinString: return vString(readString(bytes, pos));
    case kBinArray:
    case kBinSet:
    case kBinTuple: {
      std::uint64_t count = readVarint(bytes, pos);
      std::vector<Value> items;
      items.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, 1024)));
      for (std::uint64_t i = 0; i < count; ++i) items.push_back(decodeBinary(bytes, pos));
      if (tag == kBinTuple) {
        auto tuple = std::make_shared<TupleValue>();
        tuple->items = std::move(items);
        Value out;
        out.data = std::move(tuple);
        return out;
      }
      return vArray(std::move(items));
    }
    case kBinMap: {
      std::uint64_t count = readVarint(bytes, pos);
      std::vector<std::pair<Value, Value>> entries;
      for (std::uint64_t i = 0; i < count; ++i) {
        Value key = decodeBinary(bytes, pos);
        Value value = decodeBinary(bytes, pos);
        entries.emplace_back(std::move(key), std::move(value));
      }
      return vMap(std::move(entries));
    }
    case kBinStruct: {
      std::string typeName = readString(bytes, pos);
      std::uint64_t count = readVarint(bytes, pos);
      std::vector<std::pair<Value, Value>> entries;
      entries.emplace_back(vString("$type"), vString(typeName));
      for (std::uint64_t i = 0; i < count; ++i) {
        std::string name = readString(bytes, pos);
        entries.emplace_back(vString(name), decodeBinary(bytes, pos));
      }
      return vMap(std::move(entries));
    }
    case kBinNone: return vNone();
    case kBinSome: return vSome(decodeBinary(bytes, pos));
    case kBinOk: {
      auto result = std::make_shared<ResultValue>();
      result->ok = true;
      result->value = decodeBinary(bytes, pos);
      Value out;
      out.data = std::move(result);
      return out;
    }
    case kBinErr: {
      auto result = std::make_shared<ResultValue>();
      result->ok = false;
      result->value = decodeBinary(bytes, pos);
      Value out;
      out.data = std::move(result);
      return out;
    }
    default: throw BinaryFailure("unknown tag " + std::to_string(tag));
  }
}

// ----------------------------------------------------------------- logging

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

std::mutex g_logMutex;
LogLevel g_logLevel = LogLevel::Info;
bool g_logJson = false;

const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "INFO";
}

std::string utcTimestamp(std::chrono::system_clock::time_point point) {
  auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch()).count();
  std::time_t seconds = static_cast<std::time_t>(millis / 1000);
  int remainder = static_cast<int>(millis % 1000);
  if (remainder < 0) {
    remainder += 1000;
    --seconds;
  }
  std::tm tm{};
  gmtime_r(&seconds, &tm);
  char buffer[64];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
  char full[80];
  std::snprintf(full, sizeof(full), "%s.%03dZ", buffer, remainder);
  return full;
}

// Fields and labels may be given as a map, a struct, or an optional of either.
bool isFieldsValue(const Value& value) {
  if (std::holds_alternative<MapPtr>(value.data) ||
      std::holds_alternative<StructPtr>(value.data)) {
    return true;
  }
  if (std::holds_alternative<std::monostate>(value.data)) return true;
  if (auto* optional = std::get_if<OptionalPtr>(&value.data)) {
    return !(*optional)->hasValue || isFieldsValue((*optional)->value);
  }
  return false;
}

void collectFields(const Value& value, std::vector<std::pair<std::string, Value>>& out) {
  if (auto* map = std::get_if<MapPtr>(&value.data)) {
    for (const auto& entry : (*map)->entries) {
      const auto* key = std::get_if<std::string>(&entry.first.data);
      out.emplace_back(key ? *key : formatValue(entry.first, false), entry.second);
    }
    return;
  }
  if (auto* object = std::get_if<StructPtr>(&value.data)) {
    for (const auto& field : (*object)->fields) out.emplace_back(field.first, field.second);
    return;
  }
  if (std::holds_alternative<std::monostate>(value.data)) return;
  if (auto* optional = std::get_if<OptionalPtr>(&value.data)) {
    if (!(*optional)->hasValue) return;
    collectFields((*optional)->value, out);
    return;
  }
  out.emplace_back("data", value);
}

// ----------------------------------------------------------------- metrics

enum class MetricKind { Counter, Gauge, Histogram };

struct HistogramState {
  static constexpr std::size_t kBuckets = 11;
  std::array<std::uint64_t, kBuckets> counts{};
  double sum = 0.0;
  std::uint64_t count = 0;
};

struct MetricEntry {
  MetricKind kind = MetricKind::Counter;
  std::string name;
  std::vector<std::pair<std::string, std::string>> labels;
  double value = 0.0;
  HistogramState histogram;
};

std::mutex g_metricsMutex;
std::map<std::string, MetricEntry> g_metrics;

const std::array<double, HistogramState::kBuckets>& histogramBounds() {
  static const std::array<double, HistogramState::kBuckets> bounds = {
      0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
  return bounds;
}

std::string metricKey(const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& labels) {
  std::string key = name;
  if (labels.empty()) return key;
  key.push_back('{');
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (i) key.push_back(',');
    key += labels[i].first + "=\"" + labels[i].second + "\"";
  }
  key.push_back('}');
  return key;
}

std::string metricLabelText(const Value& value) {
  if (auto* number = std::get_if<double>(&value.data)) {
    char buffer[64];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), *number);
    return std::string(buffer, result.ptr);
  }
  return formatValue(value, false);
}

void collectLabels(const Value& value,
                   std::vector<std::pair<std::string, std::string>>& out) {
  std::vector<std::pair<std::string, Value>> fields;
  collectFields(value, fields);
  for (auto& field : fields) out.emplace_back(field.first, metricLabelText(field.second));
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
}

std::string formatMetricNumber(double value) {
  if (!std::isfinite(value)) return "0";
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

std::string renderLabels(const std::vector<std::pair<std::string, std::string>>& labels,
                         const std::string& extraKey = {}, const std::string& extraValue = {}) {
  std::vector<std::pair<std::string, std::string>> all = labels;
  if (!extraKey.empty()) all.emplace_back(extraKey, extraValue);
  if (all.empty()) return {};
  std::string out = "{";
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (i) out.push_back(',');
    out += all[i].first + "=\"";
    for (char c : all[i].second) {
      if (c == '\\' || c == '"') out.push_back('\\');
      if (c == '\n') {
        out += "\\n";
      } else {
        out.push_back(c);
      }
    }
    out.push_back('"');
  }
  out.push_back('}');
  return out;
}

// ----------------------------------------------------------------- tracing

struct TraceEvent {
  std::string name;
  double timeMs = 0.0;
};

struct SpanRecord {
  std::int64_t id = 0;
  std::string traceId;
  std::string name;
  std::int64_t parentId = 0;
  std::chrono::system_clock::time_point start;
  std::chrono::system_clock::time_point end;
  bool ended = false;
  std::vector<std::pair<std::string, Value>> attributes;
  std::vector<TraceEvent> events;
};

std::mutex g_traceMutex;
std::vector<SpanRecord> g_spans;
std::vector<std::int64_t> g_spanStack;
std::int64_t g_nextSpan = 1;
std::string g_traceId;
std::mt19937_64 g_traceRng{std::random_device{}()};

std::string randomHex(std::size_t bytes) {
  std::string out;
  out.reserve(bytes * 2);
  static const char* digits = "0123456789abcdef";
  for (std::size_t i = 0; i < bytes; ++i) {
    std::uint8_t byte = static_cast<std::uint8_t>(g_traceRng() & 0xFF);
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0xF]);
  }
  return out;
}

std::string hex16(std::int64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx",
                static_cast<unsigned long long>(static_cast<std::uint64_t>(value)));
  return buffer;
}

double epochMillis(std::chrono::system_clock::time_point point) {
  return std::chrono::duration<double, std::milli>(point.time_since_epoch()).count();
}

SpanRecord* findSpan(std::int64_t id) {
  for (auto& span : g_spans) {
    if (span.id == id) return &span;
  }
  return nullptr;
}

std::int64_t startSpan(const std::string& name, std::int64_t parentId,
                       const std::string& traceId) {
  SpanRecord span;
  span.id = g_nextSpan++;
  span.traceId = traceId.empty() ? (g_traceId.empty() ? (g_traceId = randomHex(16)) : g_traceId)
                                 : traceId;
  span.name = name;
  span.parentId = parentId;
  span.start = std::chrono::system_clock::now();
  g_spans.push_back(std::move(span));
  g_spanStack.push_back(g_spans.back().id);
  return g_spans.back().id;
}

void endSpan(std::int64_t id) {
  SpanRecord* span = findSpan(id);
  if (!span) return;
  if (!span->ended) {
    span->end = std::chrono::system_clock::now();
    span->ended = true;
  }
  for (std::size_t i = 0; i < g_spanStack.size(); ++i) {
    if (g_spanStack[i] != id) continue;
    g_spanStack.erase(g_spanStack.begin() + static_cast<std::ptrdiff_t>(i));
    break;
  }
}

// ------------------------------------------------------------- networking

enum class SocketKind { Tcp, Udp, Tls };

struct SocketEntry {
  int fd = -1;
  SocketKind kind = SocketKind::Tcp;
  void* ssl = nullptr;
  void* ctx = nullptr;
};

std::mutex g_socketMutex;
std::unordered_map<std::int64_t, SocketEntry> g_sockets;
std::int64_t g_nextSocket = 1;

std::int64_t registerSocket(int fd, SocketKind kind, void* ssl = nullptr, void* ctx = nullptr) {
  std::lock_guard<std::mutex> lock(g_socketMutex);
  std::int64_t handle = g_nextSocket++;
  g_sockets[handle] = SocketEntry{fd, kind, ssl, ctx};
  return handle;
}

bool lookupSocket(std::int64_t handle, SocketEntry& out) {
  std::lock_guard<std::mutex> lock(g_socketMutex);
  auto found = g_sockets.find(handle);
  if (found == g_sockets.end()) return false;
  out = found->second;
  return true;
}

bool removeSocket(std::int64_t handle, SocketEntry& out) {
  std::lock_guard<std::mutex> lock(g_socketMutex);
  auto found = g_sockets.find(handle);
  if (found == g_sockets.end()) return false;
  out = found->second;
  g_sockets.erase(found);
  return true;
}

void setSocketTimeouts(int fd, int millis) {
  timeval timeout{};
  timeout.tv_sec = millis / 1000;
  timeout.tv_usec = (millis % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

struct AddrList {
  addrinfo* head = nullptr;
  ~AddrList() {
    if (head) freeaddrinfo(head);
  }
};

bool resolveAddresses(const std::string& host, int port, int sockType, bool passive,
                      AddrList& out, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = sockType;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  std::string service = std::to_string(port);
  int status = getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints,
                           &out.head);
  if (status != 0) {
    error = gai_strerror(status);
    return false;
  }
  return true;
}

std::string numericHost(const sockaddr* address) {
  char buffer[NI_MAXHOST];
  if (getnameinfo(address, address->sa_family == AF_INET ? sizeof(sockaddr_in)
                                                        : sizeof(sockaddr_in6),
                  buffer, sizeof(buffer), nullptr, 0, NI_NUMERICHOST) != 0) {
    return {};
  }
  return buffer;
}

int createTcpListener(const std::string& host, int port, std::string& error) {
  AddrList addresses;
  if (!resolveAddresses(host, port, SOCK_STREAM, true, addresses, error)) return -1;
  for (addrinfo* entry = addresses.head; entry; entry = entry->ai_next) {
    int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (fd < 0) continue;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(fd, entry->ai_addr, entry->ai_addrlen) == 0 && listen(fd, 64) == 0) return fd;
    error = std::strerror(errno);
    close(fd);
  }
  if (error.empty()) error = "no usable address";
  return -1;
}

int createTcpConnection(const std::string& host, int port, std::string& error) {
  AddrList addresses;
  if (!resolveAddresses(host, port, SOCK_STREAM, false, addresses, error)) return -1;
  for (addrinfo* entry = addresses.head; entry; entry = entry->ai_next) {
    int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
      setSocketTimeouts(fd, 10000);
      return fd;
    }
    error = std::strerror(errno);
    close(fd);
  }
  if (error.empty()) error = "connection failed";
  return -1;
}

int createUdpSocket(std::string& error) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) error = std::strerror(errno);
  return fd;
}

int localPortOf(int fd) {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) return -1;
  if (address.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
  }
  if (address.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
  }
  return -1;
}

bool socketSendAll(const SocketEntry& entry, const std::string& data, std::string& error) {
  std::size_t sent = 0;
  while (sent < data.size()) {
#ifdef A_HAVE_OPENSSL
    if (entry.kind == SocketKind::Tls) {
      int written = SSL_write(static_cast<SSL*>(entry.ssl), data.data() + sent,
                              static_cast<int>(data.size() - sent));
      if (written <= 0) {
        error = "TLS write failed";
        int code = SSL_get_error(static_cast<SSL*>(entry.ssl), written);
        if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
          error = "TLS write would block";
        }
        return false;
      }
      sent += static_cast<std::size_t>(written);
      continue;
    }
#endif
    ssize_t written = ::send(entry.fd, data.data() + sent, data.size() - sent, 0);
    if (written < 0) {
      if (errno == EINTR) continue;
      error = std::strerror(errno);
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

bool socketReceive(const SocketEntry& entry, std::string& buffer, std::size_t maxBytes,
                   bool& eof, std::string& error) {
  std::vector<char> chunk(maxBytes);
  std::size_t received = 0;
#ifdef A_HAVE_OPENSSL
  if (entry.kind == SocketKind::Tls) {
    int result = SSL_read(static_cast<SSL*>(entry.ssl), chunk.data(),
                          static_cast<int>(chunk.size()));
    if (result <= 0) {
      int code = SSL_get_error(static_cast<SSL*>(entry.ssl), result);
      if (code == SSL_ERROR_ZERO_RETURN) {
        eof = true;
        return true;
      }
      error = "TLS read failed";
      if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
        error = "TLS read would block";
      }
      return false;
    }
    received = static_cast<std::size_t>(result);
  } else
#endif
  {
    ssize_t result = ::recv(entry.fd, chunk.data(), chunk.size(), 0);
    if (result < 0) {
      if (errno == EINTR) return socketReceive(entry, buffer, maxBytes, eof, error);
      // A non-blocking socket with nothing waiting reads as an empty result.
      if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
      error = std::strerror(errno);
      return false;
    }
    if (result == 0) {
      eof = true;
      return true;
    }
    received = static_cast<std::size_t>(result);
  }
  buffer.append(chunk.data(), received);
  return true;
}

struct HttpResponse {
  int status = 0;
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

std::string trimCopy(const std::string& text) {
  std::size_t start = 0;
  std::size_t end = text.size();
  while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r')) {
    ++start;
  }
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(start, end - start);
}

std::string lowerCopy(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool readHttpHead(const SocketEntry& entry, std::string& buffer, std::string& head,
                  std::string& error) {
  const std::string delimiter = "\r\n\r\n";
  while (true) {
    std::size_t found = buffer.find(delimiter);
    if (found != std::string::npos) {
      head = buffer.substr(0, found);
      buffer.erase(0, found + delimiter.size());
      return true;
    }
    bool eof = false;
    if (!socketReceive(entry, buffer, 8192, eof, error)) return false;
    if (eof) {
      error = "connection closed before the headers were complete";
      return false;
    }
    if (buffer.size() > 1024 * 1024) {
      error = "HTTP headers are too large";
      return false;
    }
  }
}

bool readExact(const SocketEntry& entry, std::string& buffer, std::size_t count,
               std::string& out, std::string& error) {
  while (buffer.size() < count) {
    bool eof = false;
    if (!socketReceive(entry, buffer, 8192, eof, error)) return false;
    if (eof) {
      error = "connection closed before the body was complete";
      return false;
    }
  }
  out.append(buffer.data(), count);
  buffer.erase(0, count);
  return true;
}

bool readAllUntilEof(const SocketEntry& entry, std::string& buffer, std::string& out,
                     std::string& error) {
  out += buffer;
  buffer.clear();
  while (true) {
    bool eof = false;
    if (!socketReceive(entry, buffer, 8192, eof, error)) return false;
    if (eof) return true;
    out += buffer;
    buffer.clear();
  }
}

bool readChunkedBody(const SocketEntry& entry, std::string& buffer, std::string& body,
                     std::string& error) {
  while (true) {
    std::string line;
    while (true) {
      std::size_t found = buffer.find("\r\n");
      if (found != std::string::npos) {
        line = buffer.substr(0, found);
        buffer.erase(0, found + 2);
        break;
      }
      bool eof = false;
      if (!socketReceive(entry, buffer, 8192, eof, error)) return false;
      if (eof) {
        error = "connection closed inside a chunked body";
        return false;
      }
    }
    std::size_t semi = line.find(';');
    if (semi != std::string::npos) line = line.substr(0, semi);
    const std::string sizeText = trimCopy(line);
    std::uint64_t size = 0;
    auto result =
        std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size, 16);
    if (result.ec != std::errc()) {
      error = "invalid chunk size";
      return false;
    }
    if (size == 0) {
      // Consume the trailing CRLF after the last chunk.
      std::string trailing;
      return readExact(entry, buffer, 2, trailing, error);
    }
    if (!readExact(entry, buffer, static_cast<std::size_t>(size), body, error)) return false;
    std::string crlf;
    if (!readExact(entry, buffer, 2, crlf, error)) return false;
  }
}

bool parseResponseHead(const std::string& head, HttpResponse& response, std::string& error) {
  std::size_t lineEnd = head.find("\r\n");
  std::string statusLine = lineEnd == std::string::npos ? head : head.substr(0, lineEnd);
  if (statusLine.rfind("HTTP/", 0) != 0) {
    error = "malformed HTTP status line";
    return false;
  }
  std::size_t firstSpace = statusLine.find(' ');
  if (firstSpace == std::string::npos) {
    error = "malformed HTTP status line";
    return false;
  }
  std::size_t secondSpace = statusLine.find(' ', firstSpace + 1);
  std::string code = statusLine.substr(firstSpace + 1, secondSpace == std::string::npos
                                                          ? std::string::npos
                                                          : secondSpace - firstSpace - 1);
  std::int64_t status = 0;
  auto result = std::from_chars(code.data(), code.data() + code.size(), status);
  if (result.ec != std::errc() || status < 100 || status > 999) {
    error = "invalid HTTP status code";
    return false;
  }
  response.status = static_cast<int>(status);
  if (secondSpace != std::string::npos) response.reason = statusLine.substr(secondSpace + 1);

  std::size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
  while (pos < head.size()) {
    std::size_t next = head.find("\r\n", pos);
    std::string line = next == std::string::npos ? head.substr(pos) : head.substr(pos, next - pos);
    pos = next == std::string::npos ? head.size() : next + 2;
    std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = lowerCopy(trimCopy(line.substr(0, colon)));
    std::string value = trimCopy(line.substr(colon + 1));
    response.headers.emplace_back(std::move(key), std::move(value));
  }
  return true;
}

std::string headerValue(const std::vector<std::pair<std::string, std::string>>& headers,
                        const std::string& name) {
  std::string lower = lowerCopy(name);
  for (auto it = headers.rbegin(); it != headers.rend(); ++it) {
    if (it->first == lower) return it->second;
  }
  return {};
}

bool readResponse(const SocketEntry& entry, HttpResponse& response, std::string& error) {
  std::string buffer;
  std::string head;
  if (!readHttpHead(entry, buffer, head, error)) return false;
  if (!parseResponseHead(head, response, error)) return false;

  std::string transfer = lowerCopy(headerValue(response.headers, "transfer-encoding"));
  if (transfer.find("chunked") != std::string::npos) {
    return readChunkedBody(entry, buffer, response.body, error);
  }
  std::string lengthText = headerValue(response.headers, "content-length");
  if (!lengthText.empty()) {
    std::int64_t length = 0;
    auto result = std::from_chars(lengthText.data(), lengthText.data() + lengthText.size(), length);
    if (result.ec != std::errc() || length < 0) {
      error = "invalid Content-Length";
      return false;
    }
    return readExact(entry, buffer, static_cast<std::size_t>(length), response.body, error);
  }
  return readAllUntilEof(entry, buffer, response.body, error);
}

std::string requestText(const std::string& method, const std::string& path,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        const std::string& body) {
  std::string host = headerValue(headers, "host");
  if (host.empty()) host = "localhost";
  std::string out = method + " " + (path.empty() ? "/" : path) + " HTTP/1.1\r\n";
  out += "Host: " + host + "\r\n";
  bool hasLength = false;
  for (const auto& header : headers) {
    std::string key = lowerCopy(header.first);
    if (key == "host" || key == "content-length" || key == "connection") continue;
    out += header.first + ": " + header.second + "\r\n";
    if (key == "content-length") hasLength = true;
  }
  if (!hasLength) out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  out += "Connection: close\r\n\r\n";
  out += body;
  return out;
}

const char* reasonForStatus(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "Status";
  }
}

struct ParsedUrl {
  bool tls = false;
  std::string host;
  int port = 80;
  std::string path = "/";
};

bool parseUrl(const std::string& url, ParsedUrl& out, std::string& error) {
  std::size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) {
    error = "URL must start with http:// or https://";
    return false;
  }
  std::string scheme = lowerCopy(url.substr(0, schemeEnd));
  if (scheme != "http" && scheme != "https") {
    error = "unsupported URL scheme '" + scheme + "'";
    return false;
  }
  out.tls = scheme == "https";
  out.port = out.tls ? 443 : 80;
  std::size_t pos = schemeEnd + 3;
  std::size_t pathStart = url.find('/', pos);
  std::string authority =
      pathStart == std::string::npos ? url.substr(pos) : url.substr(pos, pathStart - pos);
  out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  if (authority.empty()) {
    error = "URL is missing a host";
    return false;
  }
  std::size_t colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']') == std::string::npos) {
    std::string portText = authority.substr(colon + 1);
    std::int64_t port = 0;
    auto result = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (result.ec != std::errc() || port <= 0 || port > 65535) {
      error = "invalid port in URL";
      return false;
    }
    out.port = static_cast<int>(port);
    authority = authority.substr(0, colon);
  }
  out.host = authority;
  return true;
}

bool bytesFromValue(const Value& value, std::vector<std::uint8_t>& out) {
  if (auto* array = std::get_if<ArrayPtr>(&value.data)) {
    out.reserve((*array)->size());
    for (const Value& item : **array) {
      auto* number = std::get_if<std::int64_t>(&item.data);
      if (!number || *number < 0 || *number > 255) return false;
      out.push_back(static_cast<std::uint8_t>(*number));
    }
    return true;
  }
  return false;
}

// ------------------------------------------------------- property testing

std::mutex g_testRngMutex;
std::mt19937_64 g_testRng{std::random_device{}()};

std::uint64_t nextRandom() {
  std::lock_guard<std::mutex> lock(g_testRngMutex);
  return g_testRng();
}

}  // namespace

Value Interpreter::callStdlibBuiltin(const std::string& name, ArgList& args, Span span) {
  const std::string display = displayBuiltinName(name);

  auto expectArgs = [&](std::size_t count) {
    if (args.size() != count) {
      fail(span, "'" + display + "' expects " + std::to_string(count) + " argument(s), found " +
                     std::to_string(args.size()) + ".");
    }
  };

  auto jsonArg = [&](const Value& value, const std::string& what) {
    if (isJsonContainer(value)) return value;
    fail(span, "'" + display + "' requires " + what + ", found " + formatValue(value, true) + ".");
  };

  auto parseJsonValue = [&](const std::string& text) -> Value {
    try {
      return JsonReader(text).parse();
    } catch (const JsonFailure& failure) {
      fail(span, "Invalid JSON: " + std::string(failure.what()) + ".");
    }
  };

  auto serializeJson = [&](const Value& value, bool pretty) -> std::string {
    try {
      return writeJson(value, pretty);
    } catch (const JsonFailure& failure) {
      fail(span, "Cannot serialize value as JSON: " + std::string(failure.what()) + ".");
    }
  };

  auto headersFromValue = [&](const Value& value) {
    std::vector<std::pair<std::string, std::string>> headers;
    if (auto* map = std::get_if<MapPtr>(&value.data)) {
      for (const auto& entry : (*map)->entries) {
        const auto* key = std::get_if<std::string>(&entry.first.data);
        const auto* text = std::get_if<std::string>(&entry.second.data);
        if (!key || !text) {
          fail(span, "'" + display + "' headers must map strings to strings.");
        }
        headers.emplace_back(*key, *text);
      }
      return headers;
    }
    if (auto* object = std::get_if<StructPtr>(&value.data)) {
      for (const auto& field : (*object)->fields) {
        const auto* text = std::get_if<std::string>(&field.second.data);
        if (!text) fail(span, "'" + display + "' headers must map strings to strings.");
        headers.emplace_back(field.first, *text);
      }
      return headers;
    }
    fail(span, "'" + display + "' expects a map of headers.");
  };

  auto doHttpRequest = [&](std::int64_t handle, const std::string& method,
                           const std::string& path,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& body) -> Value {
    SocketEntry entry;
    if (!lookupSocket(handle, entry)) fail(span, "'" + display + "': unknown socket handle.");
    std::string error;
    std::string request = requestText(method, path, headers, body);
    if (!socketSendAll(entry, request, error)) {
      fail(span, "'" + display + "' failed to send the request: " + error + ".");
    }
    HttpResponse response;
    if (!readResponse(entry, response, error)) {
      fail(span, "'" + display + "' failed to read the response: " + error + ".");
    }
    return vTuple({vInt(response.status), vMapFromStrings(response.headers),
                   vString(std::move(response.body))});
  };

  // ---------------------------------------------------------------- json
  if (name == "json_parse") {
    expectArgs(1);
    return parseJsonValue(expectString(args[0], span, "The text passed to 'json.parse'"));
  }
  if (name == "json_stringify" || name == "json_pretty") {
    expectArgs(1);
    return vString(serializeJson(args[0], name == "json_pretty"));
  }
  if (name == "json_is_valid") {
    expectArgs(1);
    const std::string& text = expectString(args[0], span, "The text passed to 'json.is_valid'");
    try {
      JsonReader(text).parse();
      return vBool(true);
    } catch (const JsonFailure&) {
      return vBool(false);
    }
  }
  if (name == "json_get" || name == "json_has") {
    expectArgs(2);
    Value object = jsonArg(args[0], "a JSON object");
    const std::string& key = expectString(args[1], span, "The key passed to 'json'");
    Value found;
    if (!jsonMapLookup(object, key, found)) {
      return name == "json_get" ? vNone() : vBool(false);
    }
    return name == "json_get" ? vSome(found) : vBool(true);
  }
  if (name == "json_at") {
    expectArgs(2);
    Value object = jsonArg(args[0], "a JSON array");
    auto* array = std::get_if<ArrayPtr>(&object.data);
    if (!array) fail(span, "'json.at' requires a JSON array.");
    std::int64_t index = expectInt(args[1], span, "The index passed to 'json.at'");
    if (index < 0) index += static_cast<std::int64_t>((*array)->size());
    if (index < 0 || index >= static_cast<std::int64_t>((*array)->size())) return vNone();
    return vSome((**array)[static_cast<std::size_t>(index)]);
  }
  if (name == "json_len") {
    expectArgs(1);
    Value object = jsonArg(args[0], "a JSON value");
    if (auto* array = std::get_if<ArrayPtr>(&object.data)) return vInt((*array)->size());
    if (auto* map = std::get_if<MapPtr>(&object.data)) return vInt((*map)->entries.size());
    if (auto* object2 = std::get_if<StructPtr>(&object.data)) return vInt((*object2)->fields.size());
    if (auto* text = std::get_if<std::string>(&object.data)) {
      return vInt(static_cast<std::int64_t>(codepointCount(*text)));
    }
    fail(span, "'json.len' requires a JSON array, object, or string.");
  }
  if (name == "json_keys") {
    expectArgs(1);
    Value object = jsonArg(args[0], "a JSON object");
    std::vector<std::string> keys;
    jsonObjectKeys(object, keys);
    std::vector<Value> items;
    items.reserve(keys.size());
    for (std::string& key : keys) items.push_back(vString(std::move(key)));
    return vArray(std::move(items));
  }
  if (name == "json_type") {
    expectArgs(1);
    return vString(jsonTypeName(args[0]));
  }
  if (name == "json_as_str") {
    expectArgs(1);
    if (auto* text = std::get_if<std::string>(&args[0].data)) return vSome(vString(*text));
    if (auto* character = std::get_if<char32_t>(&args[0].data)) {
      return vSome(vString(utf8Encode(*character)));
    }
    return vNone();
  }
  if (name == "json_as_int") {
    expectArgs(1);
    if (auto* number = std::get_if<std::int64_t>(&args[0].data)) return vSome(vInt(*number));
    if (auto* number = std::get_if<double>(&args[0].data)) {
      if (std::isfinite(*number) && std::floor(*number) == *number &&
          *number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
          *number <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return vSome(vInt(static_cast<std::int64_t>(*number)));
      }
    }
    return vNone();
  }
  if (name == "json_as_float") {
    expectArgs(1);
    if (auto* number = std::get_if<double>(&args[0].data)) return vSome(vFloat(*number));
    if (auto* number = std::get_if<std::int64_t>(&args[0].data)) {
      return vSome(vFloat(static_cast<double>(*number)));
    }
    return vNone();
  }
  if (name == "json_as_bool") {
    expectArgs(1);
    if (auto* boolean = std::get_if<bool>(&args[0].data)) return vSome(vBool(*boolean));
    return vNone();
  }
  if (name == "json_as_array") {
    expectArgs(1);
    if (std::holds_alternative<ArrayPtr>(args[0].data)) return vSome(args[0]);
    return vNone();
  }
  if (name == "json_as_map") {
    expectArgs(1);
    if (std::holds_alternative<MapPtr>(args[0].data) ||
        std::holds_alternative<StructPtr>(args[0].data)) {
      return vSome(jsonToMap(args[0]));
    }
    return vNone();
  }

  // -------------------------------------------------------------- binary
  if (name == "binary_encode") {
    expectArgs(1);
    std::vector<std::uint8_t> bytes;
    try {
      encodeBinary(bytes, args[0]);
    } catch (const BinaryFailure& failure) {
      fail(span, "Cannot encode value: " + std::string(failure.what()) + ".");
    }
    std::vector<Value> items;
    items.reserve(bytes.size());
    for (std::uint8_t byte : bytes) items.push_back(vInt(byte));
    return vArray(std::move(items));
  }
  if (name == "binary_decode") {
    expectArgs(1);
    std::vector<std::uint8_t> bytes;
    if (!bytesFromValue(args[0], bytes)) {
      fail(span, "'binary.decode' expects an array of byte values (0-255).");
    }
    try {
      std::size_t pos = 0;
      Value value = decodeBinary(bytes, pos);
      if (pos != bytes.size()) return vNone();
      return vSome(std::move(value));
    } catch (const BinaryFailure&) {
      return vNone();
    }
  }

  // ------------------------------------------------------------- testing
  if (name == "testing_seed") {
    expectArgs(1);
    std::int64_t seed = expectInt(args[0], span, "The seed passed to 'testing.seed'");
    std::lock_guard<std::mutex> lock(g_testRngMutex);
    g_testRng.seed(static_cast<std::uint64_t>(seed));
    return vVoid();
  }
  if (name == "testing_for_all_int" || name == "testing_for_all_float") {
    expectArgs(4);
    const bool ints = name == "testing_for_all_int";
    std::int64_t runs = expectInt(args[0], span, "The run count");
    if (runs < 0) fail(span, "'" + display + "' requires a non-negative run count.");
    if (runs > 1000000) fail(span, "'" + display + "' is limited to 1000000 runs.");
    const Value& property = args[3];
    if (ints) {
      std::int64_t low = expectInt(args[1], span, "The low bound");
      std::int64_t high = expectInt(args[2], span, "The high bound");
      if (low > high) fail(span, "'testing.for_all_int' requires low <= high.");
      std::uniform_int_distribution<std::int64_t> distribution(low, high);
      for (std::int64_t i = 0; i < runs; ++i) {
        const std::uint64_t sample = nextRandom();
        std::int64_t pick = 0;
        {
          // Draw through a local engine seeded from the shared stream.
          std::mt19937_64 engine(sample);
          pick = distribution(engine);
        }
        Value input = vInt(pick);
        if (expectBool(callValue(property, ArgList{input}, span), span,
                       "The property passed to 'testing.for_all_int'")) {
          continue;
        }
        err_ << "Property failed after " << (i + 1) << " test(s): input = "
             << formatValue(input, false) << "\n";
        return vBool(false);
      }
      return vBool(true);
    }
    double low = expectFloat(args[1], span, "The low bound");
    double high = expectFloat(args[2], span, "The high bound");
    if (!(low <= high)) fail(span, "'testing.for_all_float' requires low <= high.");
    std::uniform_real_distribution<double> distribution(low, high);
    for (std::int64_t i = 0; i < runs; ++i) {
      std::mt19937_64 engine(nextRandom());
      Value input = vFloat(distribution(engine));
      if (expectBool(callValue(property, ArgList{input}, span), span,
                     "The property passed to 'testing.for_all_float'")) {
        continue;
      }
      err_ << "Property failed after " << (i + 1) << " test(s): input = "
           << formatValue(input, false) << "\n";
      return vBool(false);
    }
    return vBool(true);
  }
  if (name == "testing_for_all_bool") {
    expectArgs(2);
    std::int64_t runs = expectInt(args[0], span, "The run count");
    if (runs < 0) fail(span, "'testing.for_all_bool' requires a non-negative run count.");
    const Value& property = args[1];
    for (std::int64_t i = 0; i < runs; ++i) {
      Value input = vBool((nextRandom() & 1) != 0);
      if (expectBool(callValue(property, ArgList{input}, span), span,
                     "The property passed to 'testing.for_all_bool'")) {
        continue;
      }
      err_ << "Property failed after " << (i + 1) << " test(s): input = "
           << formatValue(input, false) << "\n";
      return vBool(false);
    }
    return vBool(true);
  }
  if (name == "testing_for_all_string") {
    expectArgs(3);
    std::int64_t runs = expectInt(args[0], span, "The run count");
    std::int64_t maxLength = expectInt(args[1], span, "The maximum length");
    if (runs < 0 || maxLength < 0) {
      fail(span, "'testing.for_all_string' requires non-negative counts.");
    }
    const Value& property = args[2];
    for (std::int64_t i = 0; i < runs; ++i) {
      std::uint64_t seed = nextRandom();
      std::mt19937_64 engine(seed);
      std::uniform_int_distribution<std::int64_t> lengthDistribution(0, maxLength);
      std::uniform_int_distribution<int> charDistribution(32, 126);
      std::int64_t length = lengthDistribution(engine);
      std::string text;
      text.reserve(static_cast<std::size_t>(length));
      for (std::int64_t c = 0; c < length; ++c) {
        text.push_back(static_cast<char>(charDistribution(engine)));
      }
      Value input = vString(text);
      if (expectBool(callValue(property, ArgList{input}, span), span,
                     "The property passed to 'testing.for_all_string'")) {
        continue;
      }
      err_ << "Property failed after " << (i + 1) << " test(s): input = "
           << formatValue(input, true) << "\n";
      return vBool(false);
    }
    return vBool(true);
  }
  if (name == "testing_for_all_elem") {
    expectArgs(2);
    ArrayWindow window;
    if (!arrayWindowOf(args[0], window)) {
      fail(span, "'testing.for_all_elem' expects an array.");
    }
    for (std::size_t i = 0; i < window.length; ++i) {
      Value input = (*window.storage)[window.offset + i];
      if (expectBool(callValue(args[1], ArgList{input}, span), span,
                     "The property passed to 'testing.for_all_elem'")) {
        continue;
      }
      err_ << "Property failed after " << (i + 1) << " test(s): input = "
           << formatValue(input, false) << "\n";
      return vBool(false);
    }
    return vBool(true);
  }

  // ----------------------------------------------------------------- log
  if (name == "log_set_level") {
    expectArgs(1);
    std::string level = expectString(args[0], span, "The level passed to 'log.set_level'");
    for (char& c : level) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    LogLevel parsed = LogLevel::Info;
    if (level == "debug") {
      parsed = LogLevel::Debug;
    } else if (level == "info") {
      parsed = LogLevel::Info;
    } else if (level == "warn" || level == "warning") {
      parsed = LogLevel::Warn;
    } else if (level == "error") {
      parsed = LogLevel::Error;
    } else {
      fail(span, "Unknown log level '" + level + "'.",
           "Use \"debug\", \"info\", \"warn\", or \"error\".");
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logLevel = parsed;
    return vVoid();
  }
  if (name == "log_set_json") {
    expectArgs(1);
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logJson = expectBool(args[0], span, "The flag passed to 'log.set_json'");
    return vVoid();
  }
  if (name == "log_debug" || name == "log_info" || name == "log_warn" || name == "log_error") {
    if (args.empty()) fail(span, "'" + display + "' expects a log message.");
    const std::string& message = expectString(args[0], span, "The log message");
    LogLevel level = LogLevel::Info;
    if (name == "log_debug") level = LogLevel::Debug;
    if (name == "log_warn") level = LogLevel::Warn;
    if (name == "log_error") level = LogLevel::Error;

    std::vector<std::pair<std::string, Value>> fields;
    if (args.size() == 2) {
      if (!isFieldsValue(args[1])) {
        fail(span, "'" + display +
                       "' expects a fields map or field name/value pairs, found " +
                       formatValue(args[1], true) + ".");
      }
      collectFields(args[1], fields);
    } else if (args.size() > 2) {
      if ((args.size() - 1) % 2 != 0) {
        fail(span, "'" + display +
                       "' expects a message followed by a fields map or field name/value pairs.");
      }
      for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        fields.emplace_back(expectString(args[i], span, "The log field name"), args[i + 1]);
      }
    }

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (static_cast<int>(level) < static_cast<int>(g_logLevel)) return vVoid();
    const std::string timestamp = utcTimestamp(std::chrono::system_clock::now());
    if (g_logJson) {
      std::string line = "{\"timestamp\":";
      appendEscapedString(line, timestamp);
      line += ",\"level\":";
      appendEscapedString(line, logLevelName(level));
      line += ",\"message\":";
      appendEscapedString(line, message);
      for (const auto& field : fields) {
        if (field.first == "timestamp" || field.first == "level" ||
            field.first == "message") {
          continue;
        }
        line.push_back(',');
        appendEscapedString(line, field.first);
        line.push_back(':');
        try {
          writeJsonValue(line, field.second, false, 0);
        } catch (const JsonFailure&) {
          appendEscapedString(line, formatValue(field.second, false));
        }
      }
      line += "}";
      err_ << line << "\n";
    } else {
      err_ << timestamp << " " << logLevelName(level) << " " << message;
      for (const auto& field : fields) {
        err_ << " " << field.first << "=" << formatValue(field.second, true);
      }
      err_ << "\n";
    }
    err_.flush();
    return vVoid();
  }

  // ------------------------------------------------------------- metrics
  if (name == "metrics_counter" || name == "metrics_gauge" || name == "metrics_histogram") {
    if (args.size() < 2) {
      fail(span, "'" + display + "' expects a metric name and value.");
    }
    const std::string& metricName = expectString(args[0], span, "The metric name");
    std::vector<std::pair<std::string, std::string>> labels;
    if (args.size() == 3) {
      if (!isFieldsValue(args[2])) {
        fail(span, "'" + display +
                       "' expects a labels map or label name/value pairs, found " +
                       formatValue(args[2], true) + ".");
      }
      collectLabels(args[2], labels);
    } else if (args.size() > 3) {
      if ((args.size() - 2) % 2 != 0) {
        fail(span, "'" + display +
                       "' expects a name, a value, then a labels map or label name/value pairs.");
      }
      for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
        labels.emplace_back(expectString(args[i], span, "The label name"),
                            metricLabelText(args[i + 1]));
      }
      std::sort(labels.begin(), labels.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    const std::string key = metricKey(metricName, labels);

    std::lock_guard<std::mutex> lock(g_metricsMutex);
    auto found = g_metrics.find(key);
    MetricKind kind = name == "metrics_counter"
                          ? MetricKind::Counter
                          : (name == "metrics_gauge" ? MetricKind::Gauge
                                                     : MetricKind::Histogram);
    if (found != g_metrics.end() && found->second.kind != kind) {
      fail(span, "Metric '" + metricName + "' was already registered with a different type.");
    }
    if (found == g_metrics.end()) {
      MetricEntry entry;
      entry.kind = kind;
      entry.name = metricName;
      entry.labels = labels;
      found = g_metrics.emplace(key, std::move(entry)).first;
    }
    MetricEntry& entry = found->second;
    if (kind == MetricKind::Counter) {
      const std::int64_t increment = expectInt(args[1], span, "The counter increment");
      if (increment < 0) {
        fail(span, "'metrics.counter' requires a non-negative increment; use a gauge to decrease.");
      }
      entry.value += static_cast<double>(increment);
      return vVoid();
    }
    double amount = expectFloat(args[1], span, "The metric value");
    if (kind == MetricKind::Gauge) {
      entry.value = amount;
      return vVoid();
    }
    const auto& bounds = histogramBounds();
    for (std::size_t i = 0; i < bounds.size(); ++i) {
      if (amount <= bounds[i]) {
        ++entry.histogram.counts[i];
        break;
      }
    }
    entry.histogram.sum += amount;
    ++entry.histogram.count;
    return vVoid();
  }
  if (name == "metrics_render") {
    expectArgs(0);
    std::lock_guard<std::mutex> lock(g_metricsMutex);
    std::string out;
    std::string family;
    for (const auto& item : g_metrics) {
      const MetricEntry& entry = item.second;
      if (entry.name != family) {
        const char* kindName =
            entry.kind == MetricKind::Counter
                ? "counter"
                : (entry.kind == MetricKind::Gauge ? "gauge" : "histogram");
        out += "# TYPE " + entry.name + " " + kindName + "\n";
        family = entry.name;
      }
      if (entry.kind == MetricKind::Histogram) {
        const auto& bounds = histogramBounds();
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < bounds.size(); ++i) {
          cumulative += entry.histogram.counts[i];
          out += entry.name + "_bucket" +
                 renderLabels(entry.labels, "le", formatMetricNumber(bounds[i])) + " " +
                 std::to_string(cumulative) + "\n";
        }
        out += entry.name + "_bucket" + renderLabels(entry.labels, "le", "+Inf") + " " +
               std::to_string(entry.histogram.count) + "\n";
        out += entry.name + "_sum" + renderLabels(entry.labels) + " " +
               formatMetricNumber(entry.histogram.sum) + "\n";
        out += entry.name + "_count" + renderLabels(entry.labels) + " " +
               std::to_string(entry.histogram.count) + "\n";
        continue;
      }
      out += entry.name + renderLabels(entry.labels) + " " + formatMetricNumber(entry.value) +
             "\n";
    }
    return vString(std::move(out));
  }
  if (name == "metrics_reset") {
    expectArgs(0);
    std::lock_guard<std::mutex> lock(g_metricsMutex);
    g_metrics.clear();
    return vVoid();
  }

  // --------------------------------------------------------------- trace
  if (name == "trace_start") {
    expectArgs(1);
    const std::string& spanName = expectString(args[0], span, "The span name");
    std::lock_guard<std::mutex> lock(g_traceMutex);
    std::int64_t parent = g_spanStack.empty() ? 0 : g_spanStack.back();
    return vInt(startSpan(spanName, parent, {}));
  }
  if (name == "trace_end") {
    expectArgs(1);
    std::int64_t id = expectInt(args[0], span, "The span passed to 'trace.end'");
    std::lock_guard<std::mutex> lock(g_traceMutex);
    if (!findSpan(id)) fail(span, "'trace.end': unknown span " + std::to_string(id) + ".");
    endSpan(id);
    return vVoid();
  }
  if (name == "trace_current") {
    expectArgs(0);
    std::lock_guard<std::mutex> lock(g_traceMutex);
    return vInt(g_spanStack.empty() ? 0 : g_spanStack.back());
  }
  if (name == "trace_set_attribute") {
    expectArgs(3);
    std::int64_t id = expectInt(args[0], span, "The span passed to 'trace.set_attribute'");
    const std::string& key = expectString(args[1], span, "The attribute name");
    std::lock_guard<std::mutex> lock(g_traceMutex);
    SpanRecord* record = findSpan(id);
    if (!record) fail(span, "'trace.set_attribute': unknown span " + std::to_string(id) + ".");
    for (auto& attribute : record->attributes) {
      if (attribute.first == key) {
        attribute.second = args[2];
        return vVoid();
      }
    }
    record->attributes.emplace_back(key, args[2]);
    return vVoid();
  }
  if (name == "trace_add_event") {
    expectArgs(2);
    std::int64_t id = expectInt(args[0], span, "The span passed to 'trace.add_event'");
    const std::string& eventName = expectString(args[1], span, "The event name");
    std::lock_guard<std::mutex> lock(g_traceMutex);
    SpanRecord* record = findSpan(id);
    if (!record) fail(span, "'trace.add_event': unknown span " + std::to_string(id) + ".");
    record->events.push_back(TraceEvent{eventName, epochMillis(std::chrono::system_clock::now())});
    return vVoid();
  }
  if (name == "trace_inject") {
    expectArgs(0);
    std::lock_guard<std::mutex> lock(g_traceMutex);
    if (g_spanStack.empty()) return vString("");
    std::int64_t id = g_spanStack.back();
    SpanRecord* record = findSpan(id);
    if (!record) return vString("");
    return vString("00-" + record->traceId + "-" + hex16(id) + "-01");
  }
  if (name == "trace_extract") {
    expectArgs(1);
    const std::string& context = expectString(args[0], span, "The trace context");
    if (context.size() < 55 || context[2] != '-' || context[35] != '-' || context[52] != '-') {
      fail(span, "'trace.extract' expects a W3C traceparent value.");
    }
    const std::string traceId = context.substr(3, 32);
    if (traceId.find_first_not_of("0123456789abcdef") != std::string::npos) {
      fail(span, "'trace.extract': the trace id must be lowercase hexadecimal.");
    }
    std::lock_guard<std::mutex> lock(g_traceMutex);
    return vInt(startSpan("remote", 0, traceId));
  }
  if (name == "trace_spans") {
    expectArgs(0);
    std::lock_guard<std::mutex> lock(g_traceMutex);
    std::vector<Value> items;
    items.reserve(g_spans.size());
    for (const SpanRecord& span : g_spans) {
      std::vector<std::pair<Value, Value>> entries;
      entries.emplace_back(vString("name"), vString(span.name));
      entries.emplace_back(vString("trace_id"), vString(span.traceId));
      entries.emplace_back(vString("span_id"), vString(hex16(span.id)));
      entries.emplace_back(vString("parent_id"), vInt(span.parentId));
      entries.emplace_back(vString("start_ms"), vFloat(epochMillis(span.start)));
      entries.emplace_back(vString("end_ms"),
                           span.ended ? vFloat(epochMillis(span.end)) : vFloat(0.0));
      entries.emplace_back(vString("duration_ms"),
                           span.ended ? vFloat(epochMillis(span.end) - epochMillis(span.start))
                                      : vFloat(0.0));
      std::vector<std::pair<Value, Value>> attributes;
      attributes.reserve(span.attributes.size());
      for (const auto& attribute : span.attributes) {
        attributes.emplace_back(vString(attribute.first), attribute.second);
      }
      entries.emplace_back(vString("attributes"), vMap(std::move(attributes)));
      std::vector<Value> events;
      events.reserve(span.events.size());
      for (const TraceEvent& event : span.events) {
        std::vector<std::pair<Value, Value>> eventEntries;
        eventEntries.emplace_back(vString("name"), vString(event.name));
        eventEntries.emplace_back(vString("time_ms"), vFloat(event.timeMs));
        events.push_back(vMap(std::move(eventEntries)));
      }
      entries.emplace_back(vString("events"), vArray(std::move(events)));
      items.push_back(vMap(std::move(entries)));
    }
    return vArray(std::move(items));
  }
  if (name == "trace_reset") {
    expectArgs(0);
    std::lock_guard<std::mutex> lock(g_traceMutex);
    g_spans.clear();
    g_spanStack.clear();
    g_nextSpan = 1;
    g_traceId.clear();
    return vVoid();
  }

  // ----------------------------------------------------------------- net
  if (name == "net_resolve") {
    expectArgs(1);
    const std::string& host = expectString(args[0], span, "The host name");
    AddrList addresses;
    std::string error;
    if (!resolveAddresses(host, 0, SOCK_STREAM, false, addresses, error)) {
      fail(span, "Cannot resolve '" + host + "': " + error + ".");
    }
    std::vector<Value> items;
    for (addrinfo* entry = addresses.head; entry; entry = entry->ai_next) {
      std::string text = numericHost(entry->ai_addr);
      if (text.empty()) continue;
      bool duplicate = false;
      for (const Value& existing : items) {
        if (std::get<std::string>(existing.data) == text) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) items.push_back(vString(std::move(text)));
    }
    return vArray(std::move(items));
  }
  if (name == "net_tcp_listen") {
    expectArgs(2);
    const std::string& host = expectString(args[0], span, "The host name");
    std::int64_t port = expectInt(args[1], span, "The port number");
    if (port < 0 || port > 65535) fail(span, "The port must be between 0 and 65535.");
    std::string error;
    int fd = createTcpListener(host, static_cast<int>(port), error);
    if (fd < 0) fail(span, "Cannot listen on " + host + ":" + std::to_string(port) + ": " + error + ".");
    return vInt(registerSocket(fd, SocketKind::Tcp));
  }
  if (name == "net_tcp_connect") {
    expectArgs(2);
    const std::string& host = expectString(args[0], span, "The host name");
    std::int64_t port = expectInt(args[1], span, "The port number");
    if (port <= 0 || port > 65535) fail(span, "The port must be between 1 and 65535.");
    std::string error;
    int fd = createTcpConnection(host, static_cast<int>(port), error);
    if (fd < 0) {
      fail(span, "Cannot connect to " + host + ":" + std::to_string(port) + ": " + error + ".");
    }
    return vInt(registerSocket(fd, SocketKind::Tcp));
  }
  if (name == "net_accept") {
    expectArgs(1);
    std::int64_t handle = expectInt(args[0], span, "The listening socket");
    SocketEntry server;
    if (!lookupSocket(handle, server)) fail(span, "'net.accept': unknown socket handle.");
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    int fd = -1;
    do {
      fd = accept(server.fd, reinterpret_cast<sockaddr*>(&address), &length);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) fail(span, std::string("'net.accept' failed: ") + std::strerror(errno) + ".");
    setSocketTimeouts(fd, 10000);
    return vInt(registerSocket(fd, SocketKind::Tcp));
  }
  if (name == "net_send") {
    expectArgs(2);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    const std::string& data = expectString(args[1], span, "The data to send");
    SocketEntry entry;
    if (!lookupSocket(handle, entry)) fail(span, "'net.send': unknown socket handle.");
    std::string error;
    if (!socketSendAll(entry, data, error)) fail(span, "'net.send' failed: " + error + ".");
    return vInt(static_cast<std::int64_t>(data.size()));
  }
  if (name == "net_recv") {
    expectArgs(2);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    std::int64_t maxBytes = expectInt(args[1], span, "The maximum byte count");
    if (maxBytes <= 0 || maxBytes > 64 * 1024 * 1024) {
      fail(span, "'net.recv' expects a byte count between 1 and 67108864.");
    }
    SocketEntry entry;
    if (!lookupSocket(handle, entry)) fail(span, "'net.recv': unknown socket handle.");
    std::string buffer;
    bool eof = false;
    std::string error;
    if (!socketReceive(entry, buffer, static_cast<std::size_t>(maxBytes), eof, error)) {
      fail(span, "'net.recv' failed: " + error + ".");
    }
    return vString(std::move(buffer));
  }
  if (name == "net_close") {
    expectArgs(1);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    SocketEntry entry;
    if (!removeSocket(handle, entry)) fail(span, "'net.close': unknown socket handle.");
#ifdef A_HAVE_OPENSSL
    if (entry.kind == SocketKind::Tls) {
      if (entry.ssl) {
        SSL_shutdown(static_cast<SSL*>(entry.ssl));
        SSL_free(static_cast<SSL*>(entry.ssl));
      }
      if (entry.ctx) SSL_CTX_free(static_cast<SSL_CTX*>(entry.ctx));
    }
#endif
    close(entry.fd);
    return vVoid();
  }
  if (name == "net_local_port") {
    expectArgs(1);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    SocketEntry entry;
    if (!lookupSocket(handle, entry)) fail(span, "'net.local_port': unknown socket handle.");
    int port = localPortOf(entry.fd);
    if (port < 0) fail(span, "'net.local_port' failed.");
    return vInt(port);
  }
  if (name == "net_set_timeout") {
    expectArgs(2);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    std::int64_t millis = expectInt(args[1], span, "The timeout");
    if (millis < 0) fail(span, "The timeout must be non-negative.");
    SocketEntry entry;
    if (!lookupSocket(handle, entry)) fail(span, "'net.set_timeout': unknown socket handle.");
    setSocketTimeouts(entry.fd, static_cast<int>(std::min<std::int64_t>(millis, 1'000'000)));
    return vVoid();
  }
  if (name == "net_set_nonblocking") {
    expectArgs(2);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    bool nonblocking = expectBool(args[1], span, "The non-blocking flag");
    SocketEntry entry;
    if (!lookupSocket(handle, entry)) {
      fail(span, "'net.set_nonblocking': unknown socket handle.");
    }
    int flags = fcntl(entry.fd, F_GETFL, 0);
    if (flags < 0) fail(span, std::string("'net.set_nonblocking' failed: ") + std::strerror(errno));
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(entry.fd, F_SETFL, flags) < 0) {
      fail(span, std::string("'net.set_nonblocking' failed: ") + std::strerror(errno));
    }
    return vVoid();
  }
  if (name == "net_poll") {
    expectArgs(2);
    ArrayWindow window;
    if (!arrayWindowOf(args[0], window)) {
      fail(span, "'net.poll' expects an array of socket handles.");
    }
    std::int64_t timeout = expectInt(args[1], span, "The poll timeout");
    if (timeout < -1) fail(span, "The poll timeout must be -1 or non-negative.");
    std::vector<pollfd> pollFds;
    std::vector<std::int64_t> handles;
    for (std::size_t i = 0; i < window.length; ++i) {
      auto* number = std::get_if<std::int64_t>(&(*window.storage)[window.offset + i].data);
      if (!number) fail(span, "'net.poll' expects integer socket handles.");
      SocketEntry entry;
      if (!lookupSocket(*number, entry)) {
        fail(span, "'net.poll': unknown socket handle " + std::to_string(*number) + ".");
      }
      pollfd descriptor{};
      descriptor.fd = entry.fd;
      descriptor.events = POLLIN;
      pollFds.push_back(descriptor);
      handles.push_back(*number);
    }
    int ready = 0;
    do {
      ready = poll(pollFds.data(), pollFds.size(), static_cast<int>(timeout));
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) fail(span, std::string("'net.poll' failed: ") + std::strerror(errno));
    std::vector<Value> items;
    for (std::size_t i = 0; i < pollFds.size(); ++i) {
      if (pollFds[i].revents != 0) items.push_back(vInt(handles[i]));
    }
    return vArray(std::move(items));
  }
  if (name == "net_udp_socket") {
    expectArgs(0);
    std::string error;
    int fd = createUdpSocket(error);
    if (fd < 0) fail(span, "Cannot create a UDP socket: " + error + ".");
    return vInt(registerSocket(fd, SocketKind::Udp));
  }
  if (name == "net_udp_bind") {
    expectArgs(3);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    const std::string& host = expectString(args[1], span, "The bind address");
    std::int64_t port = expectInt(args[2], span, "The port number");
    if (port < 0 || port > 65535) fail(span, "The port must be between 0 and 65535.");
    SocketEntry entry;
    if (!lookupSocket(handle, entry)) fail(span, "'net.udp_bind': unknown socket handle.");
    AddrList addresses;
    std::string error;
    if (!resolveAddresses(host, static_cast<int>(port), SOCK_DGRAM, true, addresses, error)) {
      fail(span, "Cannot resolve '" + host + "': " + error + ".");
    }
    for (addrinfo* candidate = addresses.head; candidate; candidate = candidate->ai_next) {
      if (bind(entry.fd, candidate->ai_addr, candidate->ai_addrlen) == 0) return vVoid();
      error = std::strerror(errno);
    }
    fail(span, "'net.udp_bind' failed: " + error + ".");
  }
  if (name == "net_udp_send") {
    expectArgs(4);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    const std::string& host = expectString(args[1], span, "The destination host");
    std::int64_t port = expectInt(args[2], span, "The destination port");
    const std::string& data = expectString(args[3], span, "The datagram");
    if (port <= 0 || port > 65535) fail(span, "The port must be between 1 and 65535.");
    SocketEntry entry;
    if (!lookupSocket(handle, entry)) fail(span, "'net.udp_send': unknown socket handle.");
    AddrList addresses;
    std::string error;
    if (!resolveAddresses(host, static_cast<int>(port), SOCK_DGRAM, false, addresses, error)) {
      fail(span, "Cannot resolve '" + host + "': " + error + ".");
    }
    for (addrinfo* candidate = addresses.head; candidate; candidate = candidate->ai_next) {
      ssize_t sent = sendto(entry.fd, data.data(), data.size(), 0, candidate->ai_addr,
                            candidate->ai_addrlen);
      if (sent >= 0) return vInt(sent);
      error = std::strerror(errno);
    }
    fail(span, "'net.udp_send' failed: " + error + ".");
  }
  if (name == "net_udp_recv") {
    expectArgs(2);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    std::int64_t maxBytes = expectInt(args[1], span, "The maximum byte count");
    if (maxBytes <= 0 || maxBytes > 65507) {
      fail(span, "'net.udp_recv' expects a byte count between 1 and 65507.");
    }
    SocketEntry entry;
    if (!lookupSocket(handle, entry)) fail(span, "'net.udp_recv': unknown socket handle.");
    std::vector<char> buffer(static_cast<std::size_t>(maxBytes));
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    ssize_t received = recvfrom(entry.fd, buffer.data(), buffer.size(), 0,
                                reinterpret_cast<sockaddr*>(&address), &length);
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return vTuple({vString(""), vString(""), vInt(-1)});
      }
      fail(span, std::string("'net.udp_recv' failed: ") + std::strerror(errno) + ".");
    }
    int port = 0;
    if (address.ss_family == AF_INET) port = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
    if (address.ss_family == AF_INET6) {
      port = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    }
    return vTuple({vString(std::string(buffer.data(), static_cast<std::size_t>(received))),
                   vString(numericHost(reinterpret_cast<sockaddr*>(&address))), vInt(port)});
  }
  if (name == "net_tls_available") {
    expectArgs(0);
#ifdef A_HAVE_OPENSSL
    return vBool(true);
#else
    return vBool(false);
#endif
  }
  if (name == "net_tls_connect") {
    expectArgs(2);
    const std::string& host = expectString(args[0], span, "The host name");
    std::int64_t port = expectInt(args[1], span, "The port number");
    if (port <= 0 || port > 65535) fail(span, "The port must be between 1 and 65535.");
#ifdef A_HAVE_OPENSSL
    std::string error;
    int fd = createTcpConnection(host, static_cast<int>(port), error);
    if (fd < 0) {
      fail(span, "Cannot connect to " + host + ":" + std::to_string(port) + ": " + error + ".");
    }
    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    if (!context) {
      close(fd);
      fail(span, "Cannot create a TLS context.");
    }
    SSL_CTX_set_default_verify_paths(context);
    SSL* ssl = SSL_new(context);
    if (!ssl) {
      SSL_CTX_free(context);
      close(fd);
      fail(span, "Cannot create a TLS session.");
    }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set1_host(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) {
      unsigned long code = ERR_get_error();
      std::string message = code ? ERR_error_string(code, nullptr) : "handshake failed";
      SSL_free(ssl);
      SSL_CTX_free(context);
      close(fd);
      fail(span, "TLS handshake with " + host + " failed: " + message + ".");
    }
    setSocketTimeouts(fd, 10000);
    return vInt(registerSocket(fd, SocketKind::Tls, ssl, context));
#else
    fail(span, "TLS support is not available in this build.",
         "Rebuild with OpenSSL installed so CMake can enable TLS.");
#endif
  }

  // ---------------------------------------------------------------- http
  if (name == "http_request") {
    expectArgs(5);
    std::int64_t handle = expectInt(args[0], span, "The socket handle");
    const std::string& method = expectString(args[1], span, "The HTTP method");
    const std::string& path = expectString(args[2], span, "The request path");
    auto headers = headersFromValue(args[3]);
    const std::string& body = expectString(args[4], span, "The request body");
    return doHttpRequest(handle, method, path, headers, body);
  }
  if (name == "http_get" || name == "http_post") {
    const bool post = name == "http_post";
    expectArgs(post ? 2 : 1);
    const std::string& url = expectString(args[0], span, "The URL");
    ParsedUrl parsed;
    std::string error;
    if (!parseUrl(url, parsed, error)) fail(span, "Invalid URL '" + url + "': " + error + ".");

    std::int64_t handle = 0;
    if (parsed.tls) {
#ifdef A_HAVE_OPENSSL
      int fd = createTcpConnection(parsed.host, parsed.port, error);
      if (fd < 0) fail(span, "Cannot connect to " + parsed.host + ": " + error + ".");
      SSL_CTX* context = SSL_CTX_new(TLS_client_method());
      SSL_CTX_set_default_verify_paths(context);
      SSL* ssl = context ? SSL_new(context) : nullptr;
      if (!ssl) {
        if (context) SSL_CTX_free(context);
        close(fd);
        fail(span, "Cannot create a TLS session.");
      }
      SSL_set_fd(ssl, fd);
      SSL_set_tlsext_host_name(ssl, parsed.host.c_str());
      SSL_set1_host(ssl, parsed.host.c_str());
      if (SSL_connect(ssl) != 1) {
        unsigned long code = ERR_get_error();
        std::string message = code ? ERR_error_string(code, nullptr) : "handshake failed";
        SSL_free(ssl);
        SSL_CTX_free(context);
        close(fd);
        fail(span, "TLS handshake with " + parsed.host + " failed: " + message + ".");
      }
      setSocketTimeouts(fd, 10000);
      handle = registerSocket(fd, SocketKind::Tls, ssl, context);
#else
      fail(span, "HTTPS support is not available in this build.");
#endif
    } else {
      int fd = createTcpConnection(parsed.host, parsed.port, error);
      if (fd < 0) fail(span, "Cannot connect to " + parsed.host + ": " + error + ".");
      handle = registerSocket(fd, SocketKind::Tcp);
    }

    std::vector<std::pair<std::string, std::string>> headers;
    std::string hostHeader =
        parsed.host + (parsed.port == (parsed.tls ? 443 : 80) ? "" : ":" + std::to_string(parsed.port));
    headers.emplace_back("host", hostHeader);
    if (post) headers.emplace_back("content-type", "text/plain; charset=utf-8");
    std::string body = post ? expectString(args[1], span, "The request body") : std::string();
    Value response;
    try {
      response = doHttpRequest(handle, post ? "POST" : "GET", parsed.path, headers, body);
    } catch (...) {
      SocketEntry entry;
      if (removeSocket(handle, entry)) {
#ifdef A_HAVE_OPENSSL
        if (entry.kind == SocketKind::Tls) {
          if (entry.ssl) {
            SSL_shutdown(static_cast<SSL*>(entry.ssl));
            SSL_free(static_cast<SSL*>(entry.ssl));
          }
          if (entry.ctx) SSL_CTX_free(static_cast<SSL_CTX*>(entry.ctx));
        }
#endif
        close(entry.fd);
      }
      throw;
    }
    SocketEntry entry;
    if (removeSocket(handle, entry)) {
#ifdef A_HAVE_OPENSSL
      if (entry.kind == SocketKind::Tls) {
        if (entry.ssl) {
          SSL_shutdown(static_cast<SSL*>(entry.ssl));
          SSL_free(static_cast<SSL*>(entry.ssl));
        }
        if (entry.ctx) SSL_CTX_free(static_cast<SSL_CTX*>(entry.ctx));
      }
#endif
      close(entry.fd);
    }
    return response;
  }
  if (name == "http_serve" || name == "http_serve_once") {
    expectArgs(2);
    const bool once = name == "http_serve_once";
    std::int64_t listenerHandle = expectInt(args[0], span, "The listening socket");
    SocketEntry listener;
    if (!lookupSocket(listenerHandle, listener)) {
      fail(span, "'" + display + "': unknown socket handle.");
    }
    const Value& handler = args[1];
    while (true) {
      sockaddr_storage address{};
      socklen_t length = sizeof(address);
      int clientFd = -1;
      do {
        clientFd = accept(listener.fd, reinterpret_cast<sockaddr*>(&address), &length);
      } while (clientFd < 0 && errno == EINTR);
      if (clientFd < 0) {
        fail(span, std::string("'") + display + "' accept failed: " + std::strerror(errno));
      }
      SocketEntry client{clientFd, SocketKind::Tcp, nullptr, nullptr};
      setSocketTimeouts(clientFd, 10000);

      std::string buffer;
      std::string head;
      std::string error;
      if (!readHttpHead(client, buffer, head, error)) {
        close(clientFd);
        fail(span, "'" + display + "' could not read the request: " + error + ".");
      }
      std::size_t lineEnd = head.find("\r\n");
      std::string requestLine = lineEnd == std::string::npos ? head : head.substr(0, lineEnd);
      std::size_t firstSpace = requestLine.find(' ');
      std::size_t secondSpace =
          firstSpace == std::string::npos ? std::string::npos : requestLine.find(' ', firstSpace + 1);
      if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        close(clientFd);
        fail(span, "'" + display + "' received a malformed request line.");
      }
      std::string method = requestLine.substr(0, firstSpace);
      std::string path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);

      std::vector<std::pair<std::string, std::string>> headers;
      std::size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
      while (pos < head.size()) {
        std::size_t next = head.find("\r\n", pos);
        std::string line = next == std::string::npos ? head.substr(pos) : head.substr(pos, next - pos);
        pos = next == std::string::npos ? head.size() : next + 2;
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        headers.emplace_back(lowerCopy(trimCopy(line.substr(0, colon))),
                             trimCopy(line.substr(colon + 1)));
      }

      std::string body;
      const std::string transfer = lowerCopy(headerValue(headers, "transfer-encoding"));
      if (transfer.find("chunked") != std::string::npos) {
        if (!readChunkedBody(client, buffer, body, error)) {
          close(clientFd);
          fail(span, "'" + display + "' could not read the chunked request body: " + error + ".");
        }
      }
      std::string lengthText = headerValue(headers, "content-length");
      if (body.empty() && !lengthText.empty()) {
        std::int64_t contentLength = 0;
        auto result =
            std::from_chars(lengthText.data(), lengthText.data() + lengthText.size(), contentLength);
        if (result.ec != std::errc() || contentLength < 0 ||
            contentLength > 64 * 1024 * 1024) {
          close(clientFd);
          fail(span, "'" + display + "' received an invalid Content-Length.");
        }
        if (!readExact(client, buffer, static_cast<std::size_t>(contentLength), body, error)) {
          close(clientFd);
          fail(span, "'" + display + "' could not read the request body: " + error + ".");
        }
      }

      Value result = callValue(handler,
                               ArgList{vString(method), vString(path), vMapFromStrings(headers),
                                       vString(body)},
                               span);
      auto* tuple = std::get_if<TuplePtr>(&result.data);
      if (!tuple || (*tuple)->items.size() != 3) {
        close(clientFd);
        fail(span, "'" + display + "' handler must return (status, headers, body).");
      }
      auto* status = std::get_if<std::int64_t>(&(*tuple)->items[0].data);
      auto* responseHeaders = std::get_if<MapPtr>(&(*tuple)->items[1].data);
      auto* responseBody = std::get_if<std::string>(&(*tuple)->items[2].data);
      if (!status || !responseHeaders || !responseBody) {
        close(clientFd);
        fail(span, "'" + display + "' handler returned the wrong tuple element types.");
      }
      if (*status == 0) {
        close(clientFd);
        return once ? vBool(false) : vVoid();
      }

      std::string response = "HTTP/1.1 " + std::to_string(*status) + " " +
                             reasonForStatus(static_cast<int>(*status)) + "\r\n";
      bool hasContentLength = false;
      bool chunked = false;
      for (const auto& header : (*responseHeaders)->entries) {
        const auto* key = std::get_if<std::string>(&header.first.data);
        const auto* value = std::get_if<std::string>(&header.second.data);
        if (!key || !value) {
          close(clientFd);
          fail(span, "'" + display + "' response headers must map strings to strings.");
        }
        const std::string lowered = lowerCopy(*key);
        if (lowered == "content-length") hasContentLength = true;
        if (lowered == "transfer-encoding" && lowerCopy(*value).find("chunked") != std::string::npos) {
          chunked = true;
        }
        if (lowered == "connection") continue;
        response += *key + ": " + *value + "\r\n";
      }
      if (chunked) {
        response += "\r\n";
        char size[24];
        std::snprintf(size, sizeof(size), "%llX", static_cast<unsigned long long>(responseBody->size()));
        response += size;
        response += "\r\n";
        response += *responseBody;
        response += "\r\n0\r\n\r\n";
      } else {
        if (!hasContentLength) {
          response += "Content-Length: " + std::to_string(responseBody->size()) + "\r\n";
        }
        response += "Connection: close\r\n\r\n";
        response += *responseBody;
      }
      bool sent = socketSendAll(client, response, error);
      close(clientFd);
      if (!sent) fail(span, "'" + display + "' could not write the response: " + error + ".");
      if (once) return vBool(true);
    }
  }

  fail(span, "Internal error: unknown standard-library builtin '" + name + "'.");
}

}  // namespace a
