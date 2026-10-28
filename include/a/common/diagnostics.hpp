#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

#include "a/common/source.hpp"
#include "a/common/span.hpp"

namespace a {

enum class Severity { Error, Warning };

struct Diagnostic {
  Severity severity = Severity::Error;
  Span span;
  std::string message;
  std::string help;
};

// Collects diagnostics and renders them with source context. The sink keeps
// insertion order: errors from later compiler phases are reported after
// earlier phases, matching the pipeline.
class DiagnosticSink {
 public:
  explicit DiagnosticSink(const SourceFile& source) : source_(source) {}

  void error(Span span, std::string message, std::string help = {});
  void warning(Span span, std::string message, std::string help = {});

  bool hasErrors() const { return errors_ > 0; }
  int errorCount() const { return errors_; }

  // Prints diagnostics added since the previous flush. This lets each
  // pipeline stage flush without reprinting earlier output.
  void flush(std::ostream& os, bool useColor);

 private:
  const SourceFile& source_;
  std::vector<Diagnostic> diagnostics_;
  std::size_t flushed_ = 0;
  int errors_ = 0;
};

}  // namespace a
