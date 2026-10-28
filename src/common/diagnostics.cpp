#include "a/common/diagnostics.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace a {
namespace {

struct Colors {
  const char* reset;
  const char* bold;
  const char* red;
  const char* yellow;
  const char* blue;
};

const Colors kPlain = {"", "", "", "", ""};
const Colors kAnsi = {"\x1b[0m", "\x1b[1m", "\x1b[31m", "\x1b[33m", "\x1b[34m"};

std::string repeat(char c, int count) {
  if (count <= 0) return {};
  return std::string(static_cast<std::size_t>(count), c);
}

}  // namespace

void DiagnosticSink::error(Span span, std::string message, std::string help) {
  diagnostics_.push_back(Diagnostic{Severity::Error, span, std::move(message), std::move(help)});
  ++errors_;
}

void DiagnosticSink::warning(Span span, std::string message, std::string help) {
  diagnostics_.push_back(Diagnostic{Severity::Warning, span, std::move(message), std::move(help)});
}

void DiagnosticSink::flush(std::ostream& os, bool useColor) {
  const Colors& colors = useColor ? kAnsi : kPlain;
  for (; flushed_ < diagnostics_.size(); ++flushed_) {
    const Diagnostic& diagnostic = diagnostics_[flushed_];
    const bool isError = diagnostic.severity == Severity::Error;

    os << colors.bold << (isError ? colors.red : colors.yellow)
       << (isError ? "error" : "warning") << colors.reset << colors.bold << ": "
       << diagnostic.message << colors.reset << "\n";

    if (!diagnostic.span.valid()) continue;

    const std::string lineNumber = std::to_string(diagnostic.span.line);
    const int gutter = static_cast<int>(lineNumber.size());
    os << colors.blue << "  --> " << colors.reset << source_.path() << ":"
       << diagnostic.span.line << ":" << diagnostic.span.col << "\n";
    os << colors.blue << repeat(' ', gutter + 1) << "|\n" << colors.reset;

    const std::string text = source_.lineText(diagnostic.span.line);
    os << colors.blue << lineNumber << " | " << colors.reset << text << "\n";
    os << colors.blue << repeat(' ', gutter + 1) << "| " << colors.reset;

    const int caretCol = std::max(1, diagnostic.span.col);
    int width = 1;
    if (diagnostic.span.endLine == diagnostic.span.line &&
        diagnostic.span.endCol > diagnostic.span.col) {
      width = diagnostic.span.endCol - diagnostic.span.col;
    }
    width = std::max(1, std::min(width, static_cast<int>(text.size()) - caretCol + 2));
    os << repeat(' ', caretCol - 1) << (isError ? colors.red : colors.yellow)
       << repeat('^', width) << colors.reset << "\n";

    if (!diagnostic.help.empty()) {
      os << colors.blue << repeat(' ', gutter + 1) << "| " << colors.reset << "\n";
      os << colors.blue << repeat(' ', gutter + 1) << "= " << colors.reset << colors.bold
         << "help:" << colors.reset << " " << diagnostic.help << "\n";
    }
    os << "\n";
  }
  os.flush();
}

}  // namespace a
