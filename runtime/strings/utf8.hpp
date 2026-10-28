#pragma once

#include <cstddef>
#include <string>

namespace a {

// Decodes one UTF-8 code point starting at `index` and advances `index` past
// it. Returns false once the end of the string is reached. Invalid sequences
// decode to U+FFFD and consume one byte.
bool decodeUtf8(const std::string& text, std::size_t& index, char32_t& out);

std::size_t codepointCount(const std::string& text);
char32_t codepointAt(const std::string& text, std::size_t target);
std::string utf8Encode(char32_t codepoint);

}  // namespace a
