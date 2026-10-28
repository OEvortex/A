#pragma once

#include <string>

#include "diagnostics/diagnostics.hpp"

namespace a::tooling {

struct FormatOptions {
  int indentWidth = 4;
  int maxLineWidth = 100;
};

struct FormatResult {
  bool ok = false;
  std::string text;
};

// Formats an A source buffer. Lexer/parser diagnostics are reported through
// `sink`; when any error is recorded the result's `ok` flag is false and
// `text` is empty so callers never overwrite a file with a broken program.
FormatResult formatSource(const std::string& source, DiagnosticSink& sink,
                          const FormatOptions& options = {});

}  // namespace a::tooling
