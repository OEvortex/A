#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace a {

struct DriverConfig {
  bool checkOnly = false;
  bool useColor = false;
  std::vector<std::string> modulePaths;  // -I search directories
  std::string stdlibRoot;                // --stdlib; auto-detected when empty
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

// Compiles a program to a native binary executable (`a build`).
int buildFile(const std::string& path, const std::string& outputPath,
              const DriverConfig& config, const std::string& backend, std::ostream& err);

// Emits textual LLVM IR (.ll) to a file.
int emitLLVMFile(const std::string& path, const std::string& outputPath,
                 const DriverConfig& config, std::ostream& err);

// Formats a source file. By default the formatted text is written to `out`;
// `checkOnly` reports whether the file is already formatted, and
// `writeInPlace` rewrites the input file.
struct FormatFileOptions {
  bool checkOnly = false;
  bool writeInPlace = false;
  int indentWidth = 4;
  bool useColor = false;
};

int formatFile(const std::string& path, const FormatFileOptions& options, std::ostream& out,
               std::ostream& err);

// Generates documentation for a module. Markdown is produced by default;
// `json` selects the structured JSON form, and `outputPath` writes to a file
// instead of `out`.
struct DocFileOptions {
  bool json = false;
  bool useColor = false;
  std::string outputPath;
};

int documentFile(const std::string& path, const DocFileOptions& options, std::ostream& out,
                 std::ostream& err);

}  // namespace a
