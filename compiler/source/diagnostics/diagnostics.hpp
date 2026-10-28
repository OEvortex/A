#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "lexer/source.hpp"
#include "lexer/span.hpp"

namespace a {

using FileId = int;

enum class Severity { Error, Warning };

struct Diagnostic {
  Severity severity = Severity::Error;
  Span span;
  std::string message;
  std::string help;
};

// Collects diagnostics and renders them with source context. The sink keeps
// insertion order: errors from later compiler phases are reported after
// earlier phases, matching the pipeline. It also owns the source registry used
// to map spans back to files in multi-file programs.
class DiagnosticSink {
 public:
  DiagnosticSink() = default;

  FileId addFile(std::shared_ptr<const SourceFile> source);
  const SourceFile* file(FileId id) const;

  void error(Span span, std::string message, std::string help = {});
  void warning(Span span, std::string message, std::string help = {});

  bool hasErrors() const { return errors_ > 0; }
  int errorCount() const { return errors_; }

  // Prints diagnostics added since the previous flush. This lets each
  // pipeline stage flush without reprinting earlier output.
  void flush(std::ostream& os, bool useColor);

 private:
  std::vector<std::shared_ptr<const SourceFile>> files_;
  std::vector<Diagnostic> diagnostics_;
  std::size_t flushed_ = 0;
  int errors_ = 0;
};

}  // namespace a
