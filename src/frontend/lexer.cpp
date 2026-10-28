#include "a/frontend/lexer.hpp"

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
    case Tok::KwSelf: return "'self'";
    case Tok::KwTrue: return "'true'";
    case Tok::KwFalse: return "'false'";
    case Tok::KwAnd: return "'and'";
    case Tok::KwOr: return "'or'";
    case Tok::KwNot: return "'not'";
    case Tok::KwImport: return "'import'";
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
      {"self", Tok::KwSelf},     {"true", Tok::KwTrue},     {"false", Tok::KwFalse},
      {"and", Tok::KwAnd},       {"or", Tok::KwOr},         {"not", Tok::KwNot},
      {"import", Tok::KwImport},
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

Lexer::Lexer(std::string text, DiagnosticSink& sink, int baseLine, int baseCol,
             bool inlineMode)
    : text_(std::move(text)),
      sink_(sink),
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
        case '0': value = 0; break;
        case '\\': value = '\\'; break;
        case '\'': value = '\''; break;
        case '"': value = '"'; break;
        default:
          ok = false;
          error(makeSpan(start, startLine, startCol), "Unknown escape sequence in character literal.",
                "Supported escapes: \\n \\t \\r \\0 \\\\ \\' \\\".");
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
        emitNewline();
        atLineStart_ = true;
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
        peek(1) == '=' ? emit2(Tok::EqEq) : emit1(Tok::Eq);
        break;
      case '!':
        if (peek(1) == '=') {
          emit2(Tok::NotEq);
        } else {
          advance();
          error(makeSpan(start, startLine, startCol), "'!' is not an operator.",
                "Use 'not' for logical negation, and '!=' for inequality.");
        }
        break;
      case '<':
        peek(1) == '=' ? emit2(Tok::Le) : emit1(Tok::Lt);
        break;
      case '>':
        peek(1) == '=' ? emit2(Tok::Ge) : emit1(Tok::Gt);
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
