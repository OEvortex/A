#include "tooling/source_text.hpp"

#include <algorithm>
#include <cctype>

namespace a::tooling {

SourceText::SourceText(std::string text) : text_(std::move(text)) {
  lineStarts_.push_back(0);
  for (std::size_t i = 0; i < text_.size(); ++i) {
    if (text_[i] == '\n') lineStarts_.push_back(i + 1);
  }
}

std::size_t SourceText::offsetOf(int line, int column) const {
  if (line <= 0) return 0;
  const std::size_t lineIndex = static_cast<std::size_t>(line - 1);
  if (lineIndex >= lineStarts_.size()) return text_.size();
  const std::size_t start = lineStarts_[lineIndex];
  const std::size_t columnOffset = column > 0 ? static_cast<std::size_t>(column - 1) : 0;
  return std::min(text_.size(), start + columnOffset);
}

std::string SourceText::slice(const Span& span) const {
  if (!span.valid()) return {};
  const std::size_t start = offsetOf(span.line, span.col);
  const std::size_t end = offsetOf(span.endLine > 0 ? span.endLine : span.line,
                                    span.endCol > 0 ? span.endCol : span.col);
  if (end <= start) return {};
  return text_.substr(start, end - start);
}

std::string SourceText::between(const Span& span, const Span& end) const {
  if (!span.valid() || !end.valid()) return {};
  const std::size_t start = offsetOf(span.line, span.col);
  const std::size_t stop = offsetOf(end.line, end.col);
  if (stop <= start) return {};
  return trimCopy(std::string_view(text_).substr(start, stop - start));
}

char SourceText::firstChar(const Span& span) const {
  if (!span.valid()) return '\0';
  std::size_t offset = offsetOf(span.line, span.col);
  while (offset < text_.size() &&
         (text_[offset] == ' ' || text_[offset] == '\t' || text_[offset] == '\r')) {
    ++offset;
  }
  return offset < text_.size() ? text_[offset] : '\0';
}

std::string trimCopy(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

}  // namespace a::tooling
