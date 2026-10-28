#pragma once

#include <string>
#include <vector>

#include "a/common/diagnostics.hpp"
#include "a/common/token.hpp"

namespace a {

class Lexer {
 public:
  // When `inlineMode` is true the lexer is tokenizing a fragment (such as the
  // inside of a string interpolation): no NEWLINE/INDENT/DEDENT tokens are
  // produced and physical indentation is not significant.
  Lexer(std::string text, DiagnosticSink& sink, int baseLine = 1, int baseCol = 1,
        bool inlineMode = false);

  std::vector<Token> tokenize();

 private:
  bool atEnd() const { return pos_ >= text_.size(); }
  char peek(std::size_t ahead = 0) const;
  char advance();
  bool matchChar(char expected);

  bool handleLineStart();
  void skipComment();
  void emitNewline();
  void finalize();

  void scanIdentifier();
  void scanNumber();
  void scanString();
  void scanChar();

  void add(Tok kind, std::size_t start, int startLine, int startCol);
  void addSimple(Tok kind) {
    std::size_t start = pos_;
    int line = line_;
    int col = col_;
    advance();
    add(kind, start, line, col);
  }
  void addTwo(Tok kind) {
    std::size_t start = pos_;
    int line = line_;
    int col = col_;
    advance();
    advance();
    add(kind, start, line, col);
  }
  void error(Span span, std::string message, std::string help = {});
  Span makeSpan(std::size_t start, int startLine, int startCol) const;

  std::string text_;
  DiagnosticSink& sink_;
  std::size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;
  int baseLine_;
  int baseCol_;
  bool inlineMode_ = false;
  bool atLineStart_ = true;
  int bracketDepth_ = 0;
  std::vector<int> indentStack_{0};
  std::vector<Token> tokens_;
};

}  // namespace a
