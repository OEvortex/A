#pragma once

#include <iosfwd>
#include <string>

namespace a {

struct DriverConfig {
  bool checkOnly = false;
  bool useColor = false;
};

// Compiles and, unless `checkOnly` is set, executes the named file.
// Returns a process exit code: 0 success, 1 compile error, 2 runtime error,
// 66 when the file cannot be read.
int runFile(const std::string& path, const DriverConfig& config, std::istream& in,
            std::ostream& out, std::ostream& err);

// Compiles and optionally executes in-memory source. `displayPath` is used in
// diagnostics only.
int runSource(const std::string& displayPath, std::string source, const DriverConfig& config,
              std::istream& in, std::ostream& out, std::ostream& err);

}  // namespace a
