#include "a/common/source.hpp"

#include <sstream>
#include <utility>

namespace a {

SourceFile::SourceFile(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)) {
  std::istringstream stream(text_);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines_.push_back(line);
  }
  if (lines_.empty()) lines_.push_back("");
}

std::string SourceFile::lineText(int line) const {
  if (line < 1 || line > static_cast<int>(lines_.size())) return {};
  return lines_[static_cast<std::size_t>(line - 1)];
}

}  // namespace a
