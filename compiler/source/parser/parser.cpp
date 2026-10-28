#include "a/frontend/parser.hpp"

#include <utility>

#include "a/frontend/lexer.hpp"

namespace a {
namespace {

Span spanOf(const ExprPtr& expr) { return expr ? expr->span : Span{}; }

}  // namespace

Parser::Parser(std::vector<Token> tokens, DiagnosticSink& sink)
    : tokens_(std::move(tokens)), sink_(sink) {}

const Token& Parser::peek(std::size_t ahead) const {
  std::size_t index = pos_ + ahead;
  if (index >= tokens_.size()) index = tokens_.size() - 1;
  return tokens_[index];
}

bool Parser::match(Tok kind) {
  if (!check(kind)) return false;
  advance();
  return true;
}

const Token& Parser::advance() {
  const Token& token = tokens_[pos_];
  if (pos_ + 1 < tokens_.size()) ++pos_;
  return token;
}

const Token& Parser::expect(Tok kind, const std::string& what) {
  if (check(kind)) return advance();
  fail(cur().span, "Expected " + what + ", found " + describeToken(cur()) + ".");
}

void Parser::fail(Span span, std::string message, std::string help) {
  sink_.error(span, std::move(message), std::move(help));
  throw ParseError{};
}

void Parser::synchronize() {
  while (!check(Tok::Eof)) {
    if (check(Tok::Dedent)) return;
    if (check(Tok::Newline)) {
      advance();
      return;
    }
    advance();
  }
}

std::unique_ptr<Program> Parser::parseProgram() {
  auto program = std::make_unique<Program>();
  while (!check(Tok::Eof)) {
    if (match(Tok::Newline) || match(Tok::Dedent)) continue;
    try {
      if (check(Tok::Indent)) {
        fail(cur().span, "Unexpected indentation.",
             "Only blocks introduced by ':' may be indented.");
      }
      program->statements.push_back(parseStatement());
    } catch (ParseError&) {
      synchronize();
    }
  }
  return program;
}

StmtPtr Parser::parseStatement() {
  switch (cur().kind) {
    case Tok::KwFn:
      return parseFnDecl();
    case Tok::KwStruct:
      return parseStructDecl();
    case Tok::KwVar:
      return parseVarDecl();
    case Tok::KwIf:
      return parseIf();
    case Tok::KwWhile:
      return parseWhile();
    case Tok::KwFor:
      return parseFor();
    case Tok::KwLoop:
      return parseLoop();
    case Tok::KwReturn:
      return parseReturn();
    case Tok::KwBreak:
      return parseBreak();
    case Tok::KwContinue:
      return parseContinue();
    case Tok::KwDefer:
      return parseDefer();
    case Tok::KwImport:
      return parseImport();
    case Tok::KwElif:
    case Tok::KwElse:
      fail(cur().span, std::string("Unexpected ") + tokName(cur().kind) + ".",
           "This branch has no matching 'if'.");
    case Tok::Indent:
      fail(cur().span, "Unexpected indentation.");
    default:
      break;
  }

  if (check(Tok::Ident) && peek(1).kind == Tok::Colon) {
    return parseTypedDeclaration();
  }
  return parseSimpleStatement();
}

void Parser::expectNewline() {
  if (check(Tok::Eof)) return;
  // Block-shaped expressions (match) consume their closing newline, so a
  // dedent or a just-closed block already ends the statement.
  if (check(Tok::Dedent)) return;
  if (pos_ > 0 && tokens_[pos_ - 1].kind == Tok::Dedent) return;
  expect(Tok::Newline, "end of line");
}

std::vector<StmtPtr> Parser::parseSuite() {
  std::vector<StmtPtr> body;
  if (match(Tok::Newline)) {
    expect(Tok::Indent, "an indented block");
    while (!check(Tok::Dedent) && !check(Tok::Eof)) {
      if (match(Tok::Newline)) continue;
      body.push_back(parseStatement());
    }
    expect(Tok::Dedent, "the end of the block");
  } else {
    body.push_back(parseStatement());
  }
  return body;
}

StmtPtr Parser::parseFnDecl() {
  Span start = cur().span;
  expect(Tok::KwFn, "'fn'");
  const Token& nameTok = expect(Tok::Ident, "a function name");

  auto decl = std::make_unique<FnDecl>();
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;

  expect(Tok::LParen, "'(' after the function name");
  if (!check(Tok::RParen)) {
    while (true) {
      decl->params.push_back(parseParam());
      if (!match(Tok::Comma)) break;
      if (check(Tok::RParen)) break;
    }
  }
  expect(Tok::RParen, "')' after the parameter list");

  if (match(Tok::Arrow)) {
    decl->returnType = parseTypeExpr();
  }

  // Allow either ':' or direct '=' (expression body) without requiring ':'
  if (match(Tok::Colon)) {
    if (match(Tok::Eq)) {
      auto value = parseExpression();
      auto ret = std::make_unique<ReturnStmt>();
      ret->span = spanOf(value);
      ret->value = std::move(value);
      expectNewline();
      decl->body.push_back(std::move(ret));
    } else {
      decl->body = parseSuite();
    }
  } else if (match(Tok::Eq)) {
    auto value = parseExpression();
    auto ret = std::make_unique<ReturnStmt>();
    ret->span = spanOf(value);
    ret->value = std::move(value);
    expectNewline();
    decl->body.push_back(std::move(ret));
  } else {
    fail(cur().span, "Expected ':' or '=' after function signature.");
  }

  decl->span = Span::join(start, tokens_[pos_ - 1].span);
  return decl;
}

ParamDecl Parser::parseParam() {
  ParamDecl param;
  if (check(Tok::KwSelf)) {
    param.span = cur().span;
    param.name = "self";
    advance();
  } else {
    const Token& nameTok = expect(Tok::Ident, "a parameter name");
    param.name = nameTok.lexeme;
    param.span = nameTok.span;
  }
  if (match(Tok::Colon)) {
    param.type = parseTypeExpr();
  }
  if (check(Tok::Eq)) {
    fail(cur().span, "Default parameter values are not supported yet.");
  }
  return param;
}

StmtPtr Parser::parseStructDecl() {
  Span start = cur().span;
  expect(Tok::KwStruct, "'struct'");
  const Token& nameTok = expect(Tok::Ident, "a struct name");

  auto decl = std::make_unique<StructDecl>();
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;

  expect(Tok::Colon, "':' after the struct name");
  expectNewline();
  expect(Tok::Indent, "an indented struct body");

  while (!check(Tok::Dedent) && !check(Tok::Eof)) {
    if (match(Tok::Newline)) continue;
    if (check(Tok::KwFn)) {
      auto method = parseFnDecl();
      decl->methods.push_back(std::unique_ptr<FnDecl>(static_cast<FnDecl*>(method.release())));
      continue;
    }
    const Token& fieldTok = expect(Tok::Ident, "a field or method declaration");
    FieldDecl field;
    field.name = fieldTok.lexeme;
    field.span = fieldTok.span;
    expect(Tok::Colon, "':' after the field name");
    field.type = parseTypeExpr();
    expectNewline();
    decl->fields.push_back(std::move(field));
  }
  expect(Tok::Dedent, "the end of the struct body");

  decl->span = Span::join(start, tokens_[pos_ - 1].span);
  return decl;
}

StmtPtr Parser::parseVarDecl() {
  Span start = cur().span;
  expect(Tok::KwVar, "'var'");
  if (check(Tok::LParen)) {
    return parseDestructureDecl(/*mutableBinding=*/true, start);
  }
  const Token& nameTok = expect(Tok::Ident, "a variable name");

  auto decl = std::make_unique<LetStmt>();
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;
  decl->mutableBinding = true;

  if (match(Tok::Colon)) {
    decl->type = parseTypeExpr();
  }
  expect(Tok::Eq, "'=' (variables must be initialized)");
  decl->init = parseExpression();
  expectNewline();
  decl->span = Span::join(start, tokens_[pos_ - 1].span);
  return decl;
}

StmtPtr Parser::parseTypedDeclaration() {
  Span start = cur().span;
  const Token& nameTok = advance();
  expect(Tok::Colon, "':'");
  auto type = parseTypeExpr();
  expect(Tok::Eq, "'=' (variables must be initialized)");
  auto init = parseExpression();
  expectNewline();

  auto decl = std::make_unique<LetStmt>();
  decl->span = Span::join(start, tokens_[pos_ - 1].span);
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;
  decl->type = std::move(type);
  decl->init = std::move(init);
  decl->mutableBinding = false;
  return decl;
}

StmtPtr Parser::parseDestructureDecl(bool mutableBinding, Span start) {
  expect(Tok::LParen, "'('");
  auto stmt = std::make_unique<DestructureStmt>();
  while (true) {
    const Token& nameTok = expect(Tok::Ident, "a binding name or '_'");
    stmt->names.push_back(nameTok.lexeme);
    stmt->nameSpans.push_back(nameTok.span);
    if (!match(Tok::Comma)) break;
    if (check(Tok::RParen)) break;
  }
  expect(Tok::RParen, "')' after the destructuring targets");
  expect(Tok::Eq, "'=' (variables must be initialized)");
  stmt->value = parseExpression();
  expectNewline();
  stmt->mutableBinding = mutableBinding;
  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  return stmt;
}

StmtPtr Parser::buildDestructure(ExprPtr target, ExprPtr value, bool mutableBinding,
                                 Span span) {
  auto* tuple = dynamic_cast<TupleExpr*>(target.get());
  if (!tuple) {
    fail(spanOf(target), "Only a tuple target may be destructured.");
  }
  auto stmt = std::make_unique<DestructureStmt>();
  stmt->span = span;
  stmt->mutableBinding = mutableBinding;
  for (auto& element : tuple->elements) {
    auto* ident = dynamic_cast<IdentExpr*>(element.get());
    if (!ident) {
      fail(spanOf(element),
           "Destructuring targets must be names or '_'.",
           "Write (a, b) = ... with plain names.");
    }
    stmt->names.push_back(ident->name);
    stmt->nameSpans.push_back(element->span);
  }
  stmt->value = std::move(value);
  return stmt;
}

StmtPtr Parser::parseIf() {
  Span start = cur().span;
  auto stmt = std::make_unique<IfStmt>();

  expect(Tok::KwIf, "'if'");
  auto cond = parseExpression();
  expect(Tok::Colon, "':' after the condition");
  stmt->branches.push_back(IfStmt::Branch{std::move(cond), parseSuite()});

  while (match(Tok::KwElif)) {
    auto elifCond = parseExpression();
    expect(Tok::Colon, "':' after the condition");
    stmt->branches.push_back(IfStmt::Branch{std::move(elifCond), parseSuite()});
  }

  if (match(Tok::KwElse)) {
    expect(Tok::Colon, "':' after 'else'");
    stmt->elseBody = parseSuite();
  }

  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  return stmt;
}

StmtPtr Parser::parseWhile() {
  Span start = cur().span;
  expect(Tok::KwWhile, "'while'");
  auto stmt = std::make_unique<WhileStmt>();
  stmt->cond = parseExpression();
  expect(Tok::Colon, "':' after the condition");
  stmt->body = parseSuite();
  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  return stmt;
}

StmtPtr Parser::parseFor() {
  Span start = cur().span;
  expect(Tok::KwFor, "'for'");
  const Token& varTok = expect(Tok::Ident, "a loop variable name");
  expect(Tok::KwIn, "'in' after the loop variable");
  auto stmt = std::make_unique<ForStmt>();
  stmt->var = varTok.lexeme;
  stmt->varSpan = varTok.span;
  stmt->iterable = parseExpression();
  expect(Tok::Colon, "':' after the iterable expression");
  stmt->body = parseSuite();
  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  return stmt;
}

StmtPtr Parser::parseLoop() {
  Span start = cur().span;
  expect(Tok::KwLoop, "'loop'");
  expect(Tok::Colon, "':' after 'loop'");
  auto stmt = std::make_unique<LoopStmt>();
  stmt->body = parseSuite();
  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  return stmt;
}

StmtPtr Parser::parseReturn() {
  Span start = cur().span;
  expect(Tok::KwReturn, "'return'");
  auto stmt = std::make_unique<ReturnStmt>();
  if (!check(Tok::Newline) && !check(Tok::Eof) && !check(Tok::Dedent)) {
    stmt->value = parseExpression();
  }
  expectNewline();
  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  return stmt;
}

StmtPtr Parser::parseBreak() {
  Span start = cur().span;
  expect(Tok::KwBreak, "'break'");
  auto stmt = std::make_unique<BreakStmt>();
  expectNewline();
  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  return stmt;
}

StmtPtr Parser::parseContinue() {
  Span start = cur().span;
  expect(Tok::KwContinue, "'continue'");
  auto stmt = std::make_unique<ContinueStmt>();
  expectNewline();
  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  return stmt;
}

StmtPtr Parser::parseDefer() {
  Span start = cur().span;
  expect(Tok::KwDefer, "'defer'");
  auto stmt = std::make_unique<DeferStmt>();
  stmt->body = parseStatement();
  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  return stmt;
}

StmtPtr Parser::parseImport() {
  fail(cur().span, "Imports are not supported yet.",
       "This version compiles a single source file. Modules arrive in a later milestone.");
}

StmtPtr Parser::parseSimpleStatement() {
  Span start = cur().span;
  auto target = parseExpression();
  if (isAssignmentOp(cur().kind)) {
    const Token& opTok = advance();
    auto value = parseExpression();
    expectNewline();
    if (dynamic_cast<TupleExpr*>(target.get()) != nullptr) {
      if (opTok.kind != Tok::Eq) {
        fail(opTok.span, "Destructuring only supports '='.",
             "Use separate statements to update the bound names.");
      }
      return buildDestructure(std::move(target), std::move(value),
                              /*mutableBinding=*/false,
                              Span::join(start, tokens_[pos_ - 1].span));
    }
    auto stmt = std::make_unique<AssignStmt>();
    stmt->span = Span::join(start, tokens_[pos_ - 1].span);
    stmt->op = opName(opTok.kind);
    stmt->opSpan = opTok.span;
    stmt->target = std::move(target);
    stmt->value = std::move(value);
    return stmt;
  }
  expectNewline();
  auto stmt = std::make_unique<ExprStmt>();
  stmt->span = Span::join(start, tokens_[pos_ - 1].span);
  stmt->expr = std::move(target);
  return stmt;
}

TypeExprPtr Parser::parseTypeExpr() {
  TypeExprPtr type;
  if (check(Tok::LBracket)) {
    Span start = cur().span;
    advance();
    auto elem = parseTypeExpr();
    expect(Tok::RBracket, "']' after the element type");
    auto array = std::make_unique<ArrayTypeExpr>();
    array->span = Span::join(start, tokens_[pos_ - 1].span);
    array->element = std::move(elem);
    type = std::move(array);
  } else if (check(Tok::LParen)) {
    Span start = cur().span;
    advance();
    auto first = parseTypeExpr();
    if (match(Tok::Comma)) {
      auto tuple = std::make_unique<TupleTypeExpr>();
      tuple->elements.push_back(std::move(first));
      while (!check(Tok::RParen)) {
        tuple->elements.push_back(parseTypeExpr());
        if (!match(Tok::Comma)) break;
      }
      expect(Tok::RParen, "')' after the tuple element types");
      tuple->span = Span::join(start, tokens_[pos_ - 1].span);
      type = std::move(tuple);
    } else {
      expect(Tok::RParen, "')' after the type");
      first->span = Span::join(start, tokens_[pos_ - 1].span);
      type = std::move(first);
    }
  } else if (check(Tok::KwFn)) {
    Span start = cur().span;
    advance();
    expect(Tok::LParen, "'(' after 'fn' in a function type");
    auto fn = std::make_unique<FunctionTypeExpr>();
    if (!check(Tok::RParen)) {
      while (true) {
        fn->params.push_back(parseTypeExpr());
        if (!match(Tok::Comma)) break;
        if (check(Tok::RParen)) break;
      }
    }
    expect(Tok::RParen, "')' after the function type parameters");
    expect(Tok::Arrow, "'->' before the function return type");
    fn->returnType = parseTypeExpr();
    fn->span = Span::join(start, tokens_[pos_ - 1].span);
    type = std::move(fn);
  } else {
    const Token& nameTok = expect(Tok::Ident, "a type name");
    auto named = std::make_unique<NamedTypeExpr>();
    named->span = nameTok.span;
    named->name = nameTok.lexeme;
    type = std::move(named);
  }

  while (match(Tok::Question)) {
    auto optional = std::make_unique<OptionalTypeExpr>();
    optional->span = Span::join(type->span, tokens_[pos_ - 1].span);
    optional->element = std::move(type);
    type = std::move(optional);
  }
  return type;
}

// ------------------------------------------------------------- expressions

int Parser::binaryPrecedence(Tok kind) {
  switch (kind) {
    case Tok::Pipe: return 1;
    case Tok::QuestionQuestion: return 2;
    case Tok::KwOr: return 3;
    case Tok::KwAnd: return 4;
    case Tok::EqEq:
    case Tok::NotEq:
    case Tok::Lt:
    case Tok::Gt:
    case Tok::Le:
    case Tok::Ge: return 6;
    case Tok::DotDot: return 7;
    case Tok::Plus:
    case Tok::Minus: return 8;
    case Tok::Star:
    case Tok::Slash:
    case Tok::Percent: return 9;
    case Tok::StarStar: return 11;
    default: return -1;
  }
}

bool Parser::isRightAssociative(Tok kind) { return kind == Tok::StarStar; }

const char* Parser::opName(Tok kind) {
  switch (kind) {
    case Tok::KwOr: return "or";
    case Tok::KwAnd: return "and";
    case Tok::EqEq: return "==";
    case Tok::NotEq: return "!=";
    case Tok::Lt: return "<";
    case Tok::Gt: return ">";
    case Tok::Le: return "<=";
    case Tok::Ge: return ">=";
    case Tok::DotDot: return "..";
    case Tok::QuestionQuestion: return "??";
    case Tok::Pipe: return "|>";
    case Tok::Plus: return "+";
    case Tok::Minus: return "-";
    case Tok::Star: return "*";
    case Tok::Slash: return "/";
    case Tok::Percent: return "%";
    case Tok::StarStar: return "**";
    case Tok::Eq: return "=";
    case Tok::PlusEq: return "+=";
    case Tok::MinusEq: return "-=";
    case Tok::StarEq: return "*=";
    case Tok::SlashEq: return "/=";
    case Tok::PercentEq: return "%=";
    default: return "?";
  }
}

bool Parser::isAssignmentOp(Tok kind) {
  switch (kind) {
    case Tok::Eq:
    case Tok::PlusEq:
    case Tok::MinusEq:
    case Tok::StarEq:
    case Tok::SlashEq:
    case Tok::PercentEq:
      return true;
    default:
      return false;
  }
}

ExprPtr Parser::parseExpression(int minPrec) {
  auto left = parsePrefix();
  while (true) {
    Tok kind = cur().kind;
    int prec = binaryPrecedence(kind);
    if (prec < minPrec || prec < 0) break;
    const Token& opTok = advance();
    int nextMin = isRightAssociative(kind) ? prec : prec + 1;
    auto right = parseExpression(nextMin);
    if (kind == Tok::Pipe) {
      Span leftSpan = left->span;
      auto* call = dynamic_cast<CallExpr*>(right.get());
      if (!call || dynamic_cast<IdentExpr*>(call->callee.get()) == nullptr) {
        fail(opTok.span, "The right side of '|>' must be a call to a plain function.",
             "Write value |> name(args); the value becomes the first argument.");
      }
      CallArg piped;
      piped.value = std::move(left);
      call->args.insert(call->args.begin(), std::move(piped));
      call->span = Span::join(leftSpan, call->span);
      left = std::move(right);
      continue;
    }
    auto binary = std::make_unique<BinaryExpr>();
    binary->span = Span::join(left->span, right->span);
    binary->opSpan = opTok.span;
    binary->op = opName(kind);
    binary->lhs = std::move(left);
    binary->rhs = std::move(right);
    left = std::move(binary);
  }
  return left;
}

ExprPtr Parser::parsePrefix() {
  if (check(Tok::KwNot)) {
    const Token& opTok = advance();
    auto operand = parseExpression(5);
    auto expr = std::make_unique<UnaryExpr>();
    expr->span = Span::join(opTok.span, operand->span);
    expr->opSpan = opTok.span;
    expr->op = "not";
    expr->operand = std::move(operand);
    return expr;
  }
  if (check(Tok::Minus)) {
    const Token& opTok = advance();
    auto operand = parseExpression(10);
    auto expr = std::make_unique<UnaryExpr>();
    expr->span = Span::join(opTok.span, operand->span);
    expr->opSpan = opTok.span;
    expr->op = "-";
    expr->operand = std::move(operand);
    return expr;
  }
  return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
  auto expr = parsePrimary();
  while (true) {
    if (check(Tok::Dot)) {
      advance();
      const Token& fieldTok = expect(Tok::Ident, "a field or method name");
      auto field = std::make_unique<FieldExpr>();
      field->span = Span::join(expr->span, fieldTok.span);
      field->fieldSpan = fieldTok.span;
      field->field = fieldTok.lexeme;
      field->object = std::move(expr);
      expr = std::move(field);
      continue;
    }
    if (check(Tok::QuestionDot)) {
      advance();
      const Token& fieldTok = expect(Tok::Ident, "a field name after '?.'");
      auto nav = std::make_unique<SafeNavExpr>();
      nav->span = Span::join(expr->span, fieldTok.span);
      nav->fieldSpan = fieldTok.span;
      nav->field = fieldTok.lexeme;
      nav->object = std::move(expr);
      expr = std::move(nav);
      continue;
    }
    if (check(Tok::Bang)) {
      const Token& opTok = advance();
      auto unwrap = std::make_unique<UnaryExpr>();
      unwrap->span = Span::join(expr->span, opTok.span);
      unwrap->opSpan = opTok.span;
      unwrap->op = "!";
      unwrap->operand = std::move(expr);
      expr = std::move(unwrap);
      continue;
    }
    if (check(Tok::LParen)) {
      auto args = parseCallArgs();
      auto call = std::make_unique<CallExpr>();
      call->span = Span::join(expr->span, tokens_[pos_ - 1].span);
      call->callee = std::move(expr);
      call->args = std::move(args);
      expr = std::move(call);
      continue;
    }
    if (check(Tok::LBracket)) {
      expr = parseCallOrIndexPostfix(std::move(expr));
      continue;
    }
    break;
  }
  return expr;
}

ExprPtr Parser::parseCallOrIndexPostfix(ExprPtr base) {
  Span start = cur().span;
  expect(Tok::LBracket, "'['");

  auto parseSliceTail = [&](std::unique_ptr<SliceExpr> slice) -> ExprPtr {
    if (!check(Tok::RBracket)) {
      if (check(Tok::DotDot)) {
        fail(cur().span, "Expected a slice end before '..'.",
             "Write start..end..step, e.g. items[0..10..2].");
      }
      slice->end = parseExpression(8);
      if (match(Tok::DotDot)) {
        if (check(Tok::RBracket)) {
          fail(cur().span, "Expected a slice step after '..'.");
        }
        slice->step = parseExpression(8);
        if (check(Tok::DotDot)) {
          fail(cur().span, "A slice takes at most a start, end, and step.");
        }
      }
    }
    expect(Tok::RBracket, "']' after the slice");
    slice->bracketSpan = Span::join(start, tokens_[pos_ - 1].span);
    slice->span = Span::join(slice->object->span, tokens_[pos_ - 1].span);
    return slice;
  };

  if (match(Tok::DotDot)) {
    auto slice = std::make_unique<SliceExpr>();
    slice->object = std::move(base);
    return parseSliceTail(std::move(slice));
  }

  auto first = parseExpression(8);
  if (match(Tok::DotDot)) {
    auto slice = std::make_unique<SliceExpr>();
    slice->object = std::move(base);
    slice->start = std::move(first);
    return parseSliceTail(std::move(slice));
  }

  expect(Tok::RBracket, "']' after the index");
  auto index = std::make_unique<IndexExpr>();
  index->object = std::move(base);
  index->index = std::move(first);
  index->bracketSpan = Span::join(start, tokens_[pos_ - 1].span);
  index->span = Span::join(index->object->span, tokens_[pos_ - 1].span);
  return index;
}

std::vector<CallArg> Parser::parseCallArgs() {
  std::vector<CallArg> args;
  expect(Tok::LParen, "'('");
  if (!check(Tok::RParen)) {
    while (true) {
      CallArg arg;
      if (check(Tok::Ident) && peek(1).kind == Tok::Eq) {
        arg.name = cur().lexeme;
        arg.nameSpan = cur().span;
        advance();
        advance();  // '='
      } else if (check(Tok::KwSelf) && peek(1).kind == Tok::Eq) {
        fail(cur().span, "'self' cannot be used as an argument name.");
      }
      arg.value = parseExpression();
      args.push_back(std::move(arg));
      if (!match(Tok::Comma)) break;
      if (check(Tok::RParen)) break;
    }
  }
  expect(Tok::RParen, "')' after the arguments");
  return args;
}

ExprPtr Parser::parsePrimary() {
  const Token& token = cur();
  switch (token.kind) {
    case Tok::Int: {
      advance();
      auto expr = std::make_unique<IntLitExpr>();
      expr->span = token.span;
      expr->value = token.intValue;
      return expr;
    }
    case Tok::Float: {
      advance();
      auto expr = std::make_unique<FloatLitExpr>();
      expr->span = token.span;
      expr->value = token.floatValue;
      return expr;
    }
    case Tok::String: {
      advance();
      auto expr = std::make_unique<StringLitExpr>();
      expr->span = token.span;
      expr->parts = parseStringParts(token);
      return expr;
    }
    case Tok::Char: {
      advance();
      auto expr = std::make_unique<CharLitExpr>();
      expr->span = token.span;
      expr->value = token.charValue;
      return expr;
    }
    case Tok::KwTrue:
    case Tok::KwFalse: {
      advance();
      auto expr = std::make_unique<BoolLitExpr>();
      expr->span = token.span;
      expr->value = token.kind == Tok::KwTrue;
      return expr;
    }
    case Tok::Ident: {
      advance();
      auto expr = std::make_unique<IdentExpr>();
      expr->span = token.span;
      expr->name = token.lexeme;
      return expr;
    }
    case Tok::KwSelf: {
      advance();
      auto expr = std::make_unique<IdentExpr>();
      expr->span = token.span;
      expr->name = "self";
      return expr;
    }
    case Tok::KwNone: {
      advance();
      auto expr = std::make_unique<IdentExpr>();
      expr->span = token.span;
      expr->name = "None";
      return expr;
    }
    case Tok::KwFn:
      return parseLambda();
    case Tok::KwMatch:
      return parseMatch();
    case Tok::LParen: {
      Span start = token.span;
      advance();
      if (check(Tok::RParen)) {
        fail(cur().span, "Empty tuples are not supported.");
      }
      auto first = parseExpression();
      if (!match(Tok::Comma)) {
        expect(Tok::RParen, "')' after the expression");
        first->span = Span::join(start, tokens_[pos_ - 1].span);
        return first;
      }
      auto tuple = std::make_unique<TupleExpr>();
      tuple->elements.push_back(std::move(first));
      while (!check(Tok::RParen)) {
        tuple->elements.push_back(parseExpression());
        if (!match(Tok::Comma)) break;
      }
      expect(Tok::RParen, "')' after the tuple elements");
      tuple->span = Span::join(start, tokens_[pos_ - 1].span);
      return tuple;
    }
    case Tok::LBracket: {
      Span start = token.span;
      advance();
      if (check(Tok::RBracket)) {
        advance();
        auto array = std::make_unique<ArrayLitExpr>();
        array->span = Span::join(start, tokens_[pos_ - 1].span);
        return array;
      }
      auto first = parseExpression();
      if (check(Tok::KwFor)) {
        advance();
        const Token& varTok = expect(Tok::Ident, "a loop variable name");
        expect(Tok::KwIn, "'in' after the loop variable");
        auto comprehension = std::make_unique<ComprehensionExpr>();
        comprehension->var = varTok.lexeme;
        comprehension->varSpan = varTok.span;
        comprehension->element = std::move(first);
        comprehension->iterable = parseExpression();
        if (match(Tok::KwIf)) {
          comprehension->condition = parseExpression();
        }
        expect(Tok::RBracket, "']' after the comprehension");
        comprehension->span = Span::join(start, tokens_[pos_ - 1].span);
        return comprehension;
      }
      auto array = std::make_unique<ArrayLitExpr>();
      array->elements.push_back(std::move(first));
      while (match(Tok::Comma)) {
        if (check(Tok::RBracket)) break;
        array->elements.push_back(parseExpression());
      }
      expect(Tok::RBracket, "']' after the array elements");
      array->span = Span::join(start, tokens_[pos_ - 1].span);
      return array;
    }
    default:
      fail(token.span, "Expected an expression, found " + describeToken(token) + ".");
  }
}

ExprPtr Parser::parseLambda() {
  Span start = cur().span;
  expect(Tok::KwFn, "'fn'");
  expect(Tok::LParen, "'(' after 'fn' in a lambda");
  auto lambda = std::make_unique<LambdaExpr>();
  if (!check(Tok::RParen)) {
    while (true) {
      lambda->params.push_back(parseParam());
      if (!match(Tok::Comma)) break;
      if (check(Tok::RParen)) break;
    }
  }
  expect(Tok::RParen, "')' after the lambda parameters");
  if (match(Tok::Arrow)) {
    lambda->returnType = parseTypeExpr();
  }
  expect(Tok::FatArrow, "'=>' before the lambda body");
  lambda->body = parseExpression();
  lambda->span = Span::join(start, lambda->body->span);
  return lambda;
}

Pattern Parser::parsePattern() {
  Pattern pattern;
  const Token& startTok = cur();
  pattern.span = startTok.span;
  if (check(Tok::Ident) && startTok.lexeme == "_") {
    advance();
    pattern.kind = Pattern::Kind::Wildcard;
    return pattern;
  }
  auto expr = parseExpression();
  if (auto* binary = dynamic_cast<BinaryExpr*>(expr.get()); binary && binary->op == "..") {
    pattern.kind = Pattern::Kind::Range;
    pattern.rangeStart = std::move(binary->lhs);
    pattern.rangeEnd = std::move(binary->rhs);
    pattern.span = expr->span;
    return pattern;
  }
  bool literal = dynamic_cast<IntLitExpr*>(expr.get()) ||
                 dynamic_cast<FloatLitExpr*>(expr.get()) ||
                 dynamic_cast<StringLitExpr*>(expr.get()) ||
                 dynamic_cast<CharLitExpr*>(expr.get()) ||
                 dynamic_cast<BoolLitExpr*>(expr.get());
  if (auto* unary = dynamic_cast<UnaryExpr*>(expr.get());
      unary && unary->op == "-") {
    literal = dynamic_cast<IntLitExpr*>(unary->operand.get()) ||
              dynamic_cast<FloatLitExpr*>(unary->operand.get());
  }
  if (!literal) {
    fail(expr->span, "Match patterns must be literals, integer ranges, or '_'.",
         "Ranges are inclusive, e.g. 400..499.");
  }
  pattern.kind = Pattern::Kind::Literal;
  pattern.span = expr->span;
  pattern.value = std::move(expr);
  return pattern;
}

ExprPtr Parser::parseMatch() {
  Span start = cur().span;
  expect(Tok::KwMatch, "'match'");
  auto matchExpr = std::make_unique<MatchExpr>();
  matchExpr->subject = parseExpression();
  expect(Tok::Colon, "':' after the match subject");
  expectNewline();
  expect(Tok::Indent, "an indented block of match arms");
  while (!check(Tok::Dedent) && !check(Tok::Eof)) {
    if (match(Tok::Newline)) continue;
    MatchArm arm;
    arm.pattern = parsePattern();
    if (match(Tok::KwIf)) {
      arm.guard = parseExpression();
    }
    expect(Tok::FatArrow, "'=>' after the match pattern");
    arm.result = parseExpression();
    expectNewline();
    matchExpr->arms.push_back(std::move(arm));
  }
  expect(Tok::Dedent, "the end of the match block");
  if (matchExpr->arms.empty()) {
    fail(start, "A match expression needs at least one arm.");
  }
  matchExpr->span = Span::join(start, tokens_[pos_ - 1].span);
  return matchExpr;
}

ExprPtr Parser::parseInlineExpression() {
  if (check(Tok::Eof)) {
    fail(cur().span, "Expected an expression inside string interpolation.");
  }
  auto expr = parseExpression();
  if (!check(Tok::Eof)) {
    fail(cur().span, "Unexpected " + describeToken(cur()) + " inside string interpolation.");
  }
  return expr;
}

std::string Parser::decodeEscapes(const std::string& raw, Span span) {
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= raw.size()) {
      sink_.error(span, "Unterminated escape sequence in string literal.");
      break;
    }
    char esc = raw[++i];
    switch (esc) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '0': out.push_back('\0'); break;
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      case '\'': out.push_back('\''); break;
      case '{': out.push_back('{'); break;
      case '}': out.push_back('}'); break;
      default:
        sink_.error(span, std::string("Unknown escape sequence '\\") + esc + "'.",
                    "Supported escapes: \\n \\t \\r \\0 \\\\ \\\" \\' \\{ \\}.");
        out.push_back(esc);
        break;
    }
  }
  return out;
}

std::vector<StringPart> Parser::parseStringParts(const Token& token) {
  const std::string& raw = token.strValue;
  std::vector<StringPart> parts;
  if (token.rawString) {
    StringPart part;
    part.isExpr = false;
    part.text = raw;
    if (!part.text.empty()) parts.push_back(std::move(part));
    return parts;
  }
  std::string literal;
  std::size_t i = 0;

  auto flushLiteral = [&]() {
    if (literal.empty()) return;
    StringPart part;
    part.isExpr = false;
    part.text = decodeEscapes(literal, token.span);
    literal.clear();
    parts.push_back(std::move(part));
  };

  while (i < raw.size()) {
    char c = raw[i];
    if (c == '\\') {
      if (i + 1 < raw.size()) {
        if (raw[i + 1] == '{') {
          literal += '{';
          i += 2;
          continue;
        }
        literal += c;
        literal += raw[i + 1];
        i += 2;
        continue;
      }
      literal += c;
      ++i;
      continue;
    }
    if (c != '{') {
      literal += c;
      ++i;
      continue;
    }

    // Find the matching '}' while respecting nested braces and strings.
    int depth = 1;
    bool inString = false;
    std::size_t j = i + 1;
    while (j < raw.size() && depth > 0) {
      char d = raw[j];
      if (inString) {
        if (d == '\\') {
          ++j;
        } else if (d == '"') {
          inString = false;
        }
      } else if (d == '"') {
        inString = true;
      } else if (d == '{') {
        ++depth;
      } else if (d == '}') {
        --depth;
      }
      ++j;
    }
    if (depth != 0) {
      sink_.error(Span::join(token.span, token.span),
                  "Unterminated '{' in string interpolation.",
                  "Close the expression with '}'. Write '\\{' for a literal brace.");
      break;
    }

    std::string inner = raw.substr(i + 1, j - i - 2);
    flushLiteral();

    Span base = token.span;
    base.line = token.span.line;
    base.col = token.span.col + 1 + static_cast<int>(i + 1);

    StringPart part;
    part.isExpr = true;
    try {
      Lexer subLexer(inner, sink_, base.line, base.col, /*inlineMode=*/true);
      Parser subParser(subLexer.tokenize(), sink_);
      part.expr = subParser.parseInlineExpression();
    } catch (ParseError&) {
      auto placeholder = std::make_unique<IntLitExpr>();
      placeholder->span = base;
      placeholder->value = 0;
      part.expr = std::move(placeholder);
    }
    parts.push_back(std::move(part));
    i = j;
  }

  flushLiteral();
  return parts;
}

}  // namespace a
