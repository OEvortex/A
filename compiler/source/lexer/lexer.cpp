#include "lexer/lexer.hpp"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace a {

const char* tokName(Tok kind) {
  switch (kind) {
    case Tok::Eof: return "end of file";
    case Tok::Newline: return "end of line";
    case Tok::Indent: return "indent";
    case Tok::Dedent: return "dedent";
    case Tok::Ident: return "identifier";
    case Tok::Int: return "integer literal";
    case Tok::Float: return "float literal";
    case Tok::String: return "string literal";
    case Tok::Char: return "character literal";
    case Tok::KwVar: return "'var'";
    case Tok::KwFn: return "'fn'";
    case Tok::KwIf: return "'if'";
    case Tok::KwElif: return "'elif'";
    case Tok::KwElse: return "'else'";
    case Tok::KwWhile: return "'while'";
    case Tok::KwFor: return "'for'";
    case Tok::KwIn: return "'in'";
    case Tok::KwLoop: return "'loop'";
    case Tok::KwBreak: return "'break'";
    case Tok::KwContinue: return "'continue'";
    case Tok::KwReturn: return "'return'";
    case Tok::KwDefer: return "'defer'";
    case Tok::KwStruct: return "'struct'";
    case Tok::KwClass: return "'class'";
    case Tok::KwMetaclass: return "'metaclass'";
    case Tok::KwPass: return "'pass'";
    case Tok::KwSelf: return "'self'";
    case Tok::KwTrue: return "'true'";
    case Tok::KwFalse: return "'false'";
    case Tok::KwAnd: return "'and'";
    case Tok::KwOr: return "'or'";
    case Tok::KwNot: return "'not'";
    case Tok::KwImport: return "'import'";
    case Tok::KwFrom: return "'from'";
    case Tok::KwAs: return "'as'";
    case Tok::KwMatch: return "'match'";
    case Tok::KwNone: return "'None'";
    case Tok::KwConst: return "'const'";
    case Tok::KwEnum: return "'enum'";
    case Tok::KwDo: return "'do'";
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::LBracket: return "'['";
    case Tok::RBracket: return "']'";
    case Tok::LBrace: return "'{'";
    case Tok::RBrace: return "'}'";
    case Tok::Comma: return "','";
    case Tok::Colon: return "':'";
    case Tok::Dot: return "'.'";
    case Tok::DotDot: return "'..'";
    case Tok::Arrow: return "'->'";
    case Tok::FatArrow: return "'=>'";
    case Tok::Pipe: return "'|>'";
    case Tok::Bang: return "'!'";
    case Tok::Question: return "'?'";
    case Tok::QuestionDot: return "'?.'";
    case Tok::QuestionQuestion: return "'\?\?'";
    case Tok::Amp: return "'&'";
    case Tok::Bar: return "'|'";
    case Tok::Caret: return "'^'";
    case Tok::Tilde: return "'~'";
    case Tok::Shl: return "'<<'";
    case Tok::Shr: return "'>>'";
    case Tok::Plus: return "'+'";
    case Tok::Minus: return "'-'";
    case Tok::Star: return "'*'";
    case Tok::Slash: return "'/'";
    case Tok::Percent: return "'%'";
    case Tok::StarStar: return "'**'";
    case Tok::Eq: return "'='";
    case Tok::PlusEq: return "'+='";
    case Tok::MinusEq: return "'-='";
    case Tok::StarEq: return "'*='";
    case Tok::SlashEq: return "'/='";
    case Tok::PercentEq: return "'%='";
    case Tok::AmpEq: return "'&='";
    case Tok::BarEq: return "'|='";
    case Tok::CaretEq: return "'^='";
    case Tok::ShlEq: return "'<<='";
    case Tok::ShrEq: return "'>>='";
    case Tok::EqEq: return "'=='";
    case Tok::NotEq: return "'!='";
    case Tok::Lt: return "'<'";
    case Tok::Gt: return "'>'";
    case Tok::Le: return "'<='";
    case Tok::Ge: return "'>='";
  }
  return "token";
}

std::string describeToken(const Token& token) {
  switch (token.kind) {
    case Tok::Ident:
    case Tok::Int:
    case Tok::Float:
      return "'" + token.lexeme + "'";
    case Tok::String:
      return "string literal";
    case Tok::Char:
      return "character literal";
    case Tok::Eof:
      return "end of file";
    case Tok::Newline:
      return "end of line";
    case Tok::Indent:
      return "indent";
    case Tok::Dedent:
      return "dedent";
    default:
      return tokName(token.kind);
  }
}

namespace {

const std::unordered_map<std::string, Tok>& keywordTable() {
  static const std::unordered_map<std::string, Tok> table = {
      {"var", Tok::KwVar},       {"fn", Tok::KwFn},         {"if", Tok::KwIf},
      {"elif", Tok::KwElif},     {"else", Tok::KwElse},     {"while", Tok::KwWhile},
      {"for", Tok::KwFor},       {"in", Tok::KwIn},         {"loop", Tok::KwLoop},
      {"break", Tok::KwBreak},   {"continue", Tok::KwContinue},
      {"return", Tok::KwReturn}, {"defer", Tok::KwDefer},   {"struct", Tok::KwStruct},
      {"class", Tok::KwClass},   {"Class", Tok::KwClass},   {"metaclass", Tok::KwMetaclass},
      {"Metaclass", Tok::KwMetaclass}, {"pass", Tok::KwPass},
      {"self", Tok::KwSelf},     {"true", Tok::KwTrue},     {"false", Tok::KwFalse},
      {"and", Tok::KwAnd},       {"or", Tok::KwOr},         {"not", Tok::KwNot},
      {"import", Tok::KwImport}, {"from", Tok::KwFrom},       {"as", Tok::KwAs},
      {"match", Tok::KwMatch},   {"None", Tok::KwNone},       {"const", Tok::KwConst},
      {"enum", Tok::KwEnum},     {"do", Tok::KwDo},
  };
  return table;
}

bool isIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentContinue(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

}  // namespace

Lexer::Lexer(std::string text, DiagnosticSink& sink, int fileId, int baseLine, int baseCol,
             bool inlineMode)
    : text_(std::move(text)),
      sink_(sink),
      fileId_(fileId),
      baseLine_(baseLine),
      baseCol_(baseCol),
      inlineMode_(inlineMode) {
  line_ = baseLine_;
  col_ = baseCol_;
  if (inlineMode_) atLineStart_ = false;
}

char Lexer::peek(std::size_t ahead) const {
  if (pos_ + ahead >= text_.size()) return '\0';
  return text_[pos_ + ahead];
}

char Lexer::advance() {
  char c = text_[pos_++];
  if (c == '\n') {
    ++line_;
    col_ = 1;
  } else {
    ++col_;
  }
  return c;
}

bool Lexer::matchChar(char expected) {
  if (atEnd() || peek() != expected) return false;
  advance();
  return true;
}

Span Lexer::makeSpan(std::size_t start, int startLine, int startCol) const {
  Span span;
  span.line = startLine;
  span.col = startCol;
  span.endLine = line_;
  span.endCol = col_;
  if (start >= text_.size()) {
    span.endLine = startLine;
    span.endCol = startCol;
  }
  return span;
}

void Lexer::error(Span span, std::string message, std::string help) {
  sink_.error(span, std::move(message), std::move(help));
}

void Lexer::add(Tok kind, std::size_t start, int startLine, int startCol) {
  Token token;
  token.kind = kind;
  token.lexeme = text_.substr(start, pos_ - start);
  token.span = makeSpan(start, startLine, startCol);
  tokens_.push_back(std::move(token));
}

void Lexer::skipComment() {
  while (!atEnd() && peek() != '\n') advance();
}

void Lexer::emitNewline() {
  if (tokens_.empty()) return;
  Tok last = tokens_.back().kind;
  if (last == Tok::Newline || last == Tok::Indent || last == Tok::Dedent ||
      last == Tok::Eof)
    return;
  Token token;
  token.kind = Tok::Newline;
  token.span.line = line_;
  token.span.col = col_;
  token.span.endLine = line_;
  token.span.endCol = col_ + 1;
  tokens_.push_back(std::move(token));
}

bool Lexer::handleLineStart() {
  int indent = 0;
  int indentLine = line_;
  int indentCol = col_;
  while (!atEnd() && (peek() == ' ' || peek() == '\t')) {
    if (peek() == '\t') {
      Span s;
      s.line = line_;
      s.col = col_;
      s.endLine = line_;
      s.endCol = col_ + 1;
      error(s, "Tabs are not allowed for indentation.",
            "Use spaces (4 per level) to indent blocks.");
    }
    advance();
    ++indent;
  }
  if (atEnd()) return false;

  if (peek() == '\n') {
    advance();
    return false;
  }
  if (peek() == '\r') {
    advance();
    return false;
  }
  if (peek() == '#') {
    skipComment();
    if (!atEnd() && peek() == '\n') advance();
    return false;
  }

  if (indent > indentStack_.back()) {
    indentStack_.push_back(indent);
    Token token;
    token.kind = Tok::Indent;
    token.span.line = indentLine;
    token.span.col = indentCol;
    token.span.endLine = indentLine;
    token.span.endCol = indentCol + 1;
    tokens_.push_back(std::move(token));
  } else if (indent < indentStack_.back()) {
    while (!indentStack_.empty() && indent < indentStack_.back()) {
      indentStack_.pop_back();
      Token token;
      token.kind = Tok::Dedent;
      token.span.line = line_;
      token.span.col = col_;
      token.span.endLine = line_;
      token.span.endCol = col_ + 1;
      tokens_.push_back(std::move(token));
    }
    if (indentStack_.empty() || indent != indentStack_.back()) {
      Span s;
      s.line = line_;
      s.col = 1;
      s.endLine = line_;
      s.endCol = indent + 1;
      error(s, "Inconsistent indentation.",
            "This line's indentation does not match any enclosing block.");
      indentStack_.assign(1, 0);
    }
  }
  atLineStart_ = false;
  return true;
}

void Lexer::scanIdentifier() {
  std::size_t start = pos_;
  int startLine = line_;
  int startCol = col_;
  advance();
  while (!atEnd() && isIdentContinue(peek())) advance();
  std::string lexeme = text_.substr(start, pos_ - start);
  auto it = keywordTable().find(lexeme);
  Tok kind = it != keywordTable().end() ? it->second : Tok::Ident;
  add(kind, start, startLine, startCol);
}

void Lexer::scanNumber() {
  std::size_t start = pos_;
  int startLine = line_;
  int startCol = col_;
  bool isFloat = false;

  // Check for hex (0x / 0X) or binary (0b / 0B) or octal (0o / 0O)
  if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
    advance(); // '0'
    advance(); // 'x'
    std::string hexDigits;
    while (!atEnd() && (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
      char c = advance();
      if (c != '_') hexDigits.push_back(c);
    }
    Token token;
    token.kind = Tok::Int;
    token.lexeme = text_.substr(start, pos_ - start);
    token.span = makeSpan(start, startLine, startCol);
    if (hexDigits.empty()) {
      error(token.span, "Hexadecimal literal has no digits.", "Write digits after '0x', e.g. '0xFF'.");
      token.intValue = 0;
    } else {
      errno = 0;
      char* end = nullptr;
      unsigned long long val = std::strtoull(hexDigits.c_str(), &end, 16);
      if (errno == ERANGE) {
        error(token.span, "Hexadecimal literal is too large.", "Literal must fit in 64 bits.");
        val = 0;
      }
      token.intValue = static_cast<std::int64_t>(val);
    }
    tokens_.push_back(std::move(token));
    return;
  }

  if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
    advance(); // '0'
    advance(); // 'b'
    std::string binDigits;
    while (!atEnd() && (peek() == '0' || peek() == '1' || peek() == '_')) {
      char c = advance();
      if (c != '_') binDigits.push_back(c);
    }
    Token token;
    token.kind = Tok::Int;
    token.lexeme = text_.substr(start, pos_ - start);
    token.span = makeSpan(start, startLine, startCol);
    if (binDigits.empty()) {
      error(token.span, "Binary literal has no digits.", "Write binary digits after '0b', e.g. '0b1010'.");
      token.intValue = 0;
    } else {
      errno = 0;
      char* end = nullptr;
      unsigned long long val = std::strtoull(binDigits.c_str(), &end, 2);
      if (errno == ERANGE) {
        error(token.span, "Binary literal is too large.", "Literal must fit in 64 bits.");
        val = 0;
      }
      token.intValue = static_cast<std::int64_t>(val);
    }
    tokens_.push_back(std::move(token));
    return;
  }

  while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();

  // A '.' starts a fractional part only when the next character is a digit,
  // so that `1..5` lexes as `1`, `..`, `5`.
  if (peek() == '.' && isDigit(peek(1))) {
    isFloat = true;
    advance();
    while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
  }

  if ((peek() == 'e' || peek() == 'E')) {
    std::size_t save = pos_;
    int saveLine = line_;
    int saveCol = col_;
    advance();
    if (peek() == '+' || peek() == '-') advance();
    if (isDigit(peek())) {
      isFloat = true;
      while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
    } else {
      pos_ = save;
      line_ = saveLine;
      col_ = saveCol;
    }
  }

  std::string lexeme = text_.substr(start, pos_ - start);
  std::string cleaned;
  cleaned.reserve(lexeme.size());
  for (char c : lexeme) {
    if (c != '_') cleaned.push_back(c);
  }

  Token token;
  token.kind = isFloat ? Tok::Float : Tok::Int;
  token.lexeme = lexeme;
  token.span = makeSpan(start, startLine, startCol);
  if (isFloat) {
    token.floatValue = std::strtod(cleaned.c_str(), nullptr);
  } else {
    errno = 0;
    char* end = nullptr;
    long long value = std::strtoll(cleaned.c_str(), &end, 10);
    if (errno == ERANGE) {
      error(token.span, "Integer literal is too large.", "Integer literals must fit in 64 bits.");
      value = 0;
    }
    token.intValue = value;
  }
  tokens_.push_back(std::move(token));
}

void Lexer::scanString() {
  std::size_t start = pos_;
  int startLine = line_;
  int startCol = col_;

  if (peek(1) == '"' && peek(2) == '"') {
    advance();
    advance();
    advance();
    if (peek() == '\r' && peek(1) == '\n') {
      advance();
      advance();
    } else if (peek() == '\n') {
      advance();
    }
    std::size_t contentStart = pos_;
    bool terminated = false;
    while (!atEnd()) {
      if (peek() == '"' && peek(1) == '"' && peek(2) == '"') {
        std::size_t contentEnd = pos_;
        advance();
        advance();
        advance();
        terminated = true;
        Token token;
        token.kind = Tok::String;
        token.rawString = true;
        token.span = makeSpan(start, startLine, startCol);
        token.lexeme = text_.substr(start, pos_ - start);
        token.strValue = text_.substr(contentStart, contentEnd - contentStart);
        if (!terminated) {
          error(token.span, "Unterminated triple-quoted string literal.",
                "Close the raw string with '\"\"\"'.");
        }
        tokens_.push_back(std::move(token));
        return;
      }
      advance();
    }
    Token token;
    token.kind = Tok::String;
    token.rawString = true;
    token.span = makeSpan(start, startLine, startCol);
    token.lexeme = text_.substr(start, pos_ - start);
    token.strValue = text_.substr(contentStart);
    error(token.span, "Unterminated triple-quoted string literal.",
          "Close the raw string with '\"\"\"'.");
    tokens_.push_back(std::move(token));
    return;
  }

  advance();  // opening quote
  std::string raw;
  bool terminated = false;
  while (!atEnd()) {
    char c = peek();
    if (c == '"') {
      advance();
      terminated = true;
      break;
    }
    if (c == '\n') break;
    if (c == '\\') {
      raw.push_back(c);
      advance();
      if (atEnd()) break;
      if (peek() == '\n') break;
      raw.push_back(peek());
      advance();
      continue;
    }
    raw.push_back(c);
    advance();
  }
  Token token;
  token.kind = Tok::String;
  token.span = makeSpan(start, startLine, startCol);
  token.lexeme = text_.substr(start, pos_ - start);
  token.strValue = std::move(raw);
  if (!terminated) {
    error(token.span, "Unterminated string literal.", "Add a closing '\"' on the same line.");
  }
  tokens_.push_back(std::move(token));
}

void Lexer::scanChar() {
  std::size_t start = pos_;
  int startLine = line_;
  int startCol = col_;
  advance();  // opening quote
  if (atEnd() || peek() == '\n') {
    Token token;
    token.kind = Tok::Char;
    token.span = makeSpan(start, startLine, startCol);
    error(token.span, "Unterminated character literal.", "Write a character like 'a'.");
    tokens_.push_back(std::move(token));
    return;
  }

  char32_t value = 0;
  bool ok = true;
  if (peek() == '\\') {
    advance();
    if (atEnd()) {
      ok = false;
    } else {
      char esc = advance();
      switch (esc) {
        case 'n': value = '\n'; break;
        case 't': value = '\t'; break;
        case 'r': value = '\r'; break;
        case 'a': value = '\a'; break;
        case 'b': value = '\b'; break;
        case 'f': value = '\f'; break;
        case 'v': value = '\v'; break;
        case '0': value = 0; break;
        case '\\': value = '\\'; break;
        case '\'': value = '\''; break;
        case '"': value = '"'; break;
        case 'x': {
          char32_t parsed = 0;
          int digits = 0;
          while (digits < 2 && std::isxdigit(static_cast<unsigned char>(peek()))) {
            char d = advance();
            parsed = parsed * 16 + static_cast<char32_t>(std::isdigit(static_cast<unsigned char>(d))
                                                             ? d - '0'
                                                             : (std::tolower(d) - 'a' + 10));
            ++digits;
          }
          if (digits == 0) {
            ok = false;
            error(makeSpan(start, startLine, startCol),
                  "Escape '\\x' needs two hexadecimal digits.", "Write '\\x41' for 'A'.");
          }
          value = parsed;
          break;
        }
        case 'u': {
          if (!matchChar('{')) {
            ok = false;
            error(makeSpan(start, startLine, startCol),
                  "Unicode escapes use the form '\\u{1F600}'.");
            break;
          }
          char32_t parsed = 0;
          int digits = 0;
          while (std::isxdigit(static_cast<unsigned char>(peek()))) {
            char d = advance();
            parsed = parsed * 16 + static_cast<char32_t>(std::isdigit(static_cast<unsigned char>(d))
                                                             ? d - '0'
                                                             : (std::tolower(d) - 'a' + 10));
            ++digits;
          }
          if (digits == 0 || !matchChar('}') || parsed > 0x10FFFF) {
            ok = false;
            error(makeSpan(start, startLine, startCol),
                  "Invalid Unicode escape in character literal.",
                  "Write at most six hex digits, e.g. '\\u{1F600}'.");
          }
          value = parsed;
          break;
        }
        default:
          ok = false;
          error(makeSpan(start, startLine, startCol), "Unknown escape sequence in character literal.",
                "Supported escapes: \\n \\t \\r \\a \\b \\f \\v \\0 \\xNN \\u{...} \\\\ \\' \\\".");
          value = static_cast<unsigned char>(esc);
          break;
      }
    }
  } else {
    unsigned char byte = static_cast<unsigned char>(advance());
    if (byte < 0x80) {
      value = byte;
    } else {
      ok = false;
      error(makeSpan(start, startLine, startCol),
            "Non-ASCII character literal is not supported yet.",
            "Use a string literal for Unicode text.");
      value = byte;
    }
  }

  bool closed = matchChar('\'');
  Token token;
  token.kind = Tok::Char;
  token.span = makeSpan(start, startLine, startCol);
  token.lexeme = text_.substr(start, pos_ - start);
  token.charValue = value;
  if (!closed) {
    error(token.span, "Unterminated character literal.", "Add a closing '\\'' after one character.");
  }
  if (!ok) {
    // keep going; diagnostic already reported
  }
  tokens_.push_back(std::move(token));
}

void Lexer::finalize() {
  if (tokens_.empty()) {
    Token eof;
    eof.kind = Tok::Eof;
    eof.span.line = line_;
    eof.span.col = col_;
    eof.span.endLine = line_;
    eof.span.endCol = col_;
    tokens_.push_back(std::move(eof));
    return;
  }
  Tok last = tokens_.back().kind;
  if (last != Tok::Newline && last != Tok::Dedent && last != Tok::Indent &&
      last != Tok::Eof) {
    Token token;
    token.kind = Tok::Newline;
    token.span.line = line_;
    token.span.col = col_;
    token.span.endLine = line_;
    token.span.endCol = col_ + 1;
    tokens_.push_back(std::move(token));
  }
  while (indentStack_.size() > 1) {
    indentStack_.pop_back();
    Token token;
    token.kind = Tok::Dedent;
    token.span.line = line_;
    token.span.col = col_;
    token.span.endLine = line_;
    token.span.endCol = col_ + 1;
    tokens_.push_back(std::move(token));
  }
  Token eof;
  eof.kind = Tok::Eof;
  eof.span.line = line_;
  eof.span.col = col_;
  eof.span.endLine = line_;
  eof.span.endCol = col_;
  tokens_.push_back(std::move(eof));
}

std::vector<Token> Lexer::tokenize() {
  while (!atEnd()) {
    if (!inlineMode_ && atLineStart_ && bracketDepth_ == 0) {
      if (!handleLineStart()) continue;
    }

    char c = peek();
    if (c == ' ' || c == '\t' || c == '\r') {
      advance();
      continue;
    }
    if (c == '\n') {
      advance();
      if (!inlineMode_ && bracketDepth_ == 0) {
        // A line that continues with the pipeline operator belongs to the
        // previous statement, so suppress the newline and indentation.
        std::size_t look = pos_;
        while (look < text_.size() &&
               (text_[look] == ' ' || text_[look] == '\t' || text_[look] == '\r')) {
          ++look;
        }
        bool continuation = look + 1 < text_.size() && text_[look] == '|' &&
                            text_[look + 1] == '>';
        if (!continuation) {
          emitNewline();
          atLineStart_ = true;
        }
      }
      continue;
    }
    if (c == '#') {
      skipComment();
      continue;
    }

    if (isIdentStart(c)) {
      scanIdentifier();
      continue;
    }
    if (isDigit(c)) {
      scanNumber();
      continue;
    }

    std::size_t start = pos_;
    int startLine = line_;
    int startCol = col_;

    char ch = peek();
    auto emit1 = [&](Tok kind) {
      advance();
      add(kind, start, startLine, startCol);
    };
    auto emit2 = [&](Tok kind) {
      advance();
      advance();
      add(kind, start, startLine, startCol);
    };
    auto emit3 = [&](Tok kind) {
      advance();
      advance();
      advance();
      add(kind, start, startLine, startCol);
    };
    switch (ch) {
      case '"':
        scanString();
        break;
      case '\'':
        scanChar();
        break;
      case '(':
        ++bracketDepth_;
        emit1(Tok::LParen);
        break;
      case ')':
        if (bracketDepth_ > 0) --bracketDepth_;
        emit1(Tok::RParen);
        break;
      case '[':
        ++bracketDepth_;
        emit1(Tok::LBracket);
        break;
      case ']':
        if (bracketDepth_ > 0) --bracketDepth_;
        emit1(Tok::RBracket);
        break;
      case '{':
        ++bracketDepth_;
        emit1(Tok::LBrace);
        break;
      case '}':
        if (bracketDepth_ > 0) --bracketDepth_;
        emit1(Tok::RBrace);
        break;
      case ',':
        emit1(Tok::Comma);
        break;
      case ':':
        emit1(Tok::Colon);
        break;
      case '+':
        peek(1) == '=' ? emit2(Tok::PlusEq) : emit1(Tok::Plus);
        break;
      case '-':
        peek(1) == '=' ? emit2(Tok::MinusEq)
                       : peek(1) == '>' ? emit2(Tok::Arrow) : emit1(Tok::Minus);
        break;
      case '*':
        peek(1) == '*' ? emit2(Tok::StarStar)
                       : peek(1) == '=' ? emit2(Tok::StarEq) : emit1(Tok::Star);
        break;
      case '/':
        peek(1) == '=' ? emit2(Tok::SlashEq) : emit1(Tok::Slash);
        break;
      case '%':
        peek(1) == '=' ? emit2(Tok::PercentEq) : emit1(Tok::Percent);
        break;
      case '=':
        peek(1) == '>' ? emit2(Tok::FatArrow)
                       : peek(1) == '=' ? emit2(Tok::EqEq) : emit1(Tok::Eq);
        break;
      case '!':
        peek(1) == '=' ? emit2(Tok::NotEq) : emit1(Tok::Bang);
        break;
      case '?':
        peek(1) == '.' ? emit2(Tok::QuestionDot)
                       : peek(1) == '?' ? emit2(Tok::QuestionQuestion) : emit1(Tok::Question);
        break;
      case '|':
        if (peek(1) == '>') {
          emit2(Tok::Pipe);
        } else if (peek(1) == '=') {
          emit2(Tok::BarEq);
        } else {
          emit1(Tok::Bar);
        }
        break;
      case '&':
        peek(1) == '=' ? emit2(Tok::AmpEq) : emit1(Tok::Amp);
        break;
      case '^':
        peek(1) == '=' ? emit2(Tok::CaretEq) : emit1(Tok::Caret);
        break;
      case '~':
        emit1(Tok::Tilde);
        break;
      case '<':
        if (peek(1) == '<') {
          peek(2) == '=' ? emit3(Tok::ShlEq) : emit2(Tok::Shl);
        } else {
          peek(1) == '=' ? emit2(Tok::Le) : emit1(Tok::Lt);
        }
        break;
      case '>':
        if (peek(1) == '>') {
          peek(2) == '=' ? emit3(Tok::ShrEq) : emit2(Tok::Shr);
        } else {
          peek(1) == '=' ? emit2(Tok::Ge) : emit1(Tok::Gt);
        }
        break;
      case '.':
        peek(1) == '.' ? emit2(Tok::DotDot) : emit1(Tok::Dot);
        break;
      default: {
        advance();
        char bad[2] = {ch, '\0'};
        std::string shown;
        if (static_cast<unsigned char>(ch) >= 32 && static_cast<unsigned char>(ch) < 127) {
          shown = std::string("'") + bad + "'";
        } else {
          shown = "this byte";
        }
        error(makeSpan(start, startLine, startCol), "Unexpected character " + shown + ".");
        break;
      }
    }
  }

  for (auto& token : tokens_) token.span.fileId = fileId_;

  if (inlineMode_) {
    Token eof;
    eof.kind = Tok::Eof;
    eof.span.line = line_;
    eof.span.col = col_;
    eof.span.endLine = line_;
    eof.span.endCol = col_;
    tokens_.push_back(std::move(eof));
    return tokens_;
  }

  finalize();
  return tokens_;
}

}  // namespace a
