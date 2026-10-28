#pragma once

#include <cstdint>
#include <string>

#include "a/common/span.hpp"

namespace a {

enum class Tok {
  Eof,
  Newline,
  Indent,
  Dedent,

  Ident,
  Int,
  Float,
  String,
  Char,

  KwVar,
  KwFn,
  KwIf,
  KwElif,
  KwElse,
  KwWhile,
  KwFor,
  KwIn,
  KwLoop,
  KwBreak,
  KwContinue,
  KwReturn,
  KwDefer,
  KwStruct,
  KwSelf,
  KwTrue,
  KwFalse,
  KwAnd,
  KwOr,
  KwNot,
  KwImport,
  KwMatch,
  KwNone,

  LParen,
  RParen,
  LBracket,
  RBracket,
  LBrace,
  RBrace,
  Comma,
  Colon,
  Dot,
  DotDot,
  Arrow,
  FatArrow,
  Pipe,

  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  StarStar,
  Bang,
  Question,
  QuestionDot,
  QuestionQuestion,

  Eq,
  PlusEq,
  MinusEq,
  StarEq,
  SlashEq,
  PercentEq,

  EqEq,
  NotEq,
  Lt,
  Gt,
  Le,
  Ge,
};

struct Token {
  Tok kind = Tok::Eof;
  std::string lexeme;
  Span span;
  std::string strValue;  // raw contents of a string literal, escapes intact
  std::int64_t intValue = 0;
  double floatValue = 0.0;
  char32_t charValue = 0;
  bool rawString = false;  // triple-quoted literal: no escapes/interpolation
};

const char* tokName(Tok kind);

std::string describeToken(const Token& token);

}  // namespace a
