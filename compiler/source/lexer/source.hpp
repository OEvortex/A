#pragma once

#include <string>
#include <vector>

namespace a {

// Immutable source text plus cached line boundaries for diagnostics.
class SourceFile {
 public:
  SourceFile(std::string path, std::string text);

  const std::string& path() const { return path_; }
  const std::string& text() const { return text_; }

  // Returns the text of a 1-based line without its line terminator. Out of
  // range requests return an empty string.
  std::string lineText(int line) const;
  int lineCount() const { return static_cast<int>(lines_.size()); }

 private:
  std::string path_;
  std::string text_;
  std::vector<std::string> lines_;
};

}  // namespace a
