#pragma once

#include <string>

namespace a {

// Small file helpers shared by the interpreter's file builtins. Errors carry
// the operating-system message so diagnostics stay actionable.
struct FileResult {
  bool ok = false;
  std::string value;
  std::string error;
};

FileResult readFile(const std::string& path);
FileResult writeFile(const std::string& path, const std::string& contents, bool append);
bool fileExists(const std::string& path);

}  // namespace a
