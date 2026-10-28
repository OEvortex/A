#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lexer/span.hpp"

namespace a::tooling {

// Byte-oriented view over a source buffer that can map 1-based line/column
// spans (the representation used by the lexer and AST) back to substrings.
class SourceText {
 public:
  explicit SourceText(std::string text);

  const std::string& text() const { return text_; }
  std::size_t lineCount() const { return lineStarts_.size(); }

  // Byte offset of a 1-based line/column pair. Out-of-range values clamp to the
  // end of the buffer.
  std::size_t offsetOf(int line, int column) const;

  // Source slice covered by a half-open span. Returns an empty string for an
  // invalid span.
  std::string slice(const Span& span) const;

  // Text from the start of `span` up to the start of `end`, trimmed. This is
  // used to recover syntax that the AST normalizes away, such as the `pvt`,
  // `const`, `class`, `struct`, and `metaclass` keywords.
  std::string between(const Span& span, const Span& end) const;

  // First non-space character at the start of `span`, or '\0'.
  char firstChar(const Span& span) const;

 private:
  std::string text_;
  std::vector<std::size_t> lineStarts_;
};

std::string trimCopy(std::string_view text);

}  // namespace a::tooling
