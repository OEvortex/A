#pragma once

namespace a {

// A half-open source region. Lines and columns are 1-based. An invalid span
// (line == 0) means "no particular source location". `fileId` indexes the
// DiagnosticSink file registry.
struct Span {
  int line = 0;
  int col = 0;
  int endLine = 0;
  int endCol = 0;
  int fileId = 0;

  bool valid() const { return line > 0; }

  static Span join(const Span& a, const Span& b) {
    Span span = a;
    if (b.valid()) {
      span.endLine = b.endLine;
      span.endCol = b.endCol;
    }
    return span;
  }
};

}  // namespace a
