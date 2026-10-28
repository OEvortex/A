#include "strings/utf8.hpp"

namespace a {

bool decodeUtf8(const std::string& text, std::size_t& index, char32_t& out) {
  if (index >= text.size()) return false;
  const unsigned char first = static_cast<unsigned char>(text[index]);
  if (first < 0x80) {
    out = first;
    ++index;
    return true;
  }
  int extra = 0;
  char32_t value = 0;
  if ((first & 0xE0) == 0xC0) {
    extra = 1;
    value = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    extra = 2;
    value = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    extra = 3;
    value = first & 0x07;
  } else {
    out = 0xFFFD;
    ++index;
    return true;
  }
  if (index + static_cast<std::size_t>(extra) >= text.size()) {
    out = 0xFFFD;
    ++index;
    return true;
  }
  for (int i = 1; i <= extra; ++i) {
    const unsigned char byte =
        static_cast<unsigned char>(text[index + static_cast<std::size_t>(i)]);
    if ((byte & 0xC0) != 0x80) {
      out = 0xFFFD;
      ++index;
      return true;
    }
    value = (value << 6) | (byte & 0x3F);
  }
  index += static_cast<std::size_t>(extra) + 1;
  out = value;
  return true;
}

std::size_t codepointCount(const std::string& text) {
  std::size_t count = 0;
  std::size_t index = 0;
  char32_t codepoint = 0;
  while (decodeUtf8(text, index, codepoint)) ++count;
  return count;
}

char32_t codepointAt(const std::string& text, std::size_t target) {
  std::size_t index = 0;
  char32_t codepoint = 0;
  std::size_t current = 0;
  while (decodeUtf8(text, index, codepoint)) {
    if (current == target) return codepoint;
    ++current;
  }
  return 0;
}

std::string utf8Encode(char32_t codepoint) {
  std::string out;
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return out;
}

}  // namespace a
