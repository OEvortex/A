#pragma once

#include <memory>
#include <string>
#include <vector>

#include "diagnostics/diagnostics.hpp"
#include "lexer/token.hpp"
#include "ast/ast.hpp"

namespace a {

class Parser {
 public:
  Parser(std::vector<Token> tokens, DiagnosticSink& sink);

  std::unique_ptr<Program> parseProgram();

  // Parses a single expression (used for the fragments inside string
  // interpolation). Reports diagnostics and returns an error placeholder on
  // failure.
  ExprPtr parseInlineExpression();

 private:
  struct ParseError {};

  const Token& cur() const { return tokens_[pos_]; }
  const Token& peek(std::size_t ahead) const;
  bool check(Tok kind) const { return cur().kind == kind; }
  bool match(Tok kind);
  const Token& advance();
  const Token& expect(Tok kind, const std::string& what);

  [[noreturn]] void fail(Span span, std::string message, std::string help = {});
  void synchronize();

  StmtPtr parseStatement();
  StmtPtr parseFnDecl();
  StmtPtr parseStructDecl();
  StmtPtr parseEnumDecl();
  StmtPtr parseVarDecl();
  StmtPtr parseConstDecl();
  StmtPtr parseIf();
  StmtPtr parseWhile();
  StmtPtr parseDoWhile();
  StmtPtr parseFor();
  StmtPtr parseLoop();
  StmtPtr parseReturn();
  StmtPtr parseBreak();
  StmtPtr parseContinue();
  StmtPtr parseDefer();
  StmtPtr parseImport();
  StmtPtr parseFromImport();
  StmtPtr parseTypedDeclaration();
  StmtPtr parseSimpleStatement();

  std::vector<StmtPtr> parseSuite();
  void expectNewline();

  ParamDecl parseParam();
  TypeExprPtr parseTypeExpr();

  ExprPtr parseExpression(int minPrec = 0);
  ExprPtr parsePrefix();
  ExprPtr parsePostfix();
  ExprPtr parsePrimary();
  ExprPtr parseLambda();
  ExprPtr parseMatch();
  Pattern parsePattern();
  ExprPtr parseCallOrIndexPostfix(ExprPtr base);
  std::vector<CallArg> parseCallArgs();
  std::vector<StringPart> parseStringParts(const Token& token);
  std::string decodeEscapes(const std::string& raw, Span span);

  StmtPtr parseDestructureDecl(bool mutableBinding, Span start);
  StmtPtr buildDestructure(ExprPtr target, ExprPtr value, bool mutableBinding, Span span);

  static int binaryPrecedence(Tok kind);
  static bool isRightAssociative(Tok kind);
  static const char* opName(Tok kind);
  static BinOp binOp(Tok kind);
  static bool isAssignmentOp(Tok kind);

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
  DiagnosticSink& sink_;
};

}  // namespace a
