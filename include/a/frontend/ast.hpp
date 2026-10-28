#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "a/common/span.hpp"

namespace a {

struct Expr;
struct Stmt;
struct TypeExpr;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using TypeExprPtr = std::unique_ptr<TypeExpr>;

// ---------------------------------------------------------------- types

struct TypeExpr {
  Span span;
  virtual ~TypeExpr() = default;
};

struct NamedTypeExpr : TypeExpr {
  std::string name;
};

struct ArrayTypeExpr : TypeExpr {
  TypeExprPtr element;
};

// ---------------------------------------------------------------- expressions

struct Expr {
  Span span;
  virtual ~Expr() = default;
};

struct IntLitExpr : Expr {
  std::int64_t value = 0;
};

struct FloatLitExpr : Expr {
  double value = 0.0;
};

struct BoolLitExpr : Expr {
  bool value = false;
};

struct CharLitExpr : Expr {
  char32_t value = 0;
};

struct StringPart {
  bool isExpr = false;
  std::string text;  // decoded literal text
  ExprPtr expr;
};

struct StringLitExpr : Expr {
  std::vector<StringPart> parts;
};

struct IdentExpr : Expr {
  std::string name;
};

struct UnaryExpr : Expr {
  std::string op;  // "-" or "not"
  Span opSpan;
  ExprPtr operand;
};

struct BinaryExpr : Expr {
  std::string op;
  Span opSpan;
  ExprPtr lhs;
  ExprPtr rhs;
};

struct CallArg {
  std::string name;  // empty for positional arguments
  Span nameSpan;
  ExprPtr value;
};

struct CallExpr : Expr {
  ExprPtr callee;
  std::vector<CallArg> args;
};

struct IndexExpr : Expr {
  ExprPtr object;
  ExprPtr index;
  Span bracketSpan;
};

struct SliceExpr : Expr {
  ExprPtr object;
  ExprPtr start;  // may be null
  ExprPtr end;    // may be null
  Span bracketSpan;
};

struct FieldExpr : Expr {
  ExprPtr object;
  std::string field;
  Span fieldSpan;
};

struct ArrayLitExpr : Expr {
  std::vector<ExprPtr> elements;
};

struct RangeExpr : Expr {
  ExprPtr start;
  ExprPtr end;
};

// ---------------------------------------------------------------- statements

struct Stmt {
  Span span;
  virtual ~Stmt() = default;
};

struct LetStmt : Stmt {
  std::string name;
  Span nameSpan;
  TypeExprPtr type;  // may be null
  bool mutableBinding = false;
  ExprPtr init;
};

struct AssignStmt : Stmt {
  std::string op;  // "=", "+=", ...
  Span opSpan;
  ExprPtr target;
  ExprPtr value;
};

struct ExprStmt : Stmt {
  ExprPtr expr;
};

struct ReturnStmt : Stmt {
  ExprPtr value;  // may be null
};

struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};

struct DeferStmt : Stmt {
  StmtPtr body;
};

struct IfStmt : Stmt {
  struct Branch {
    ExprPtr cond;
    std::vector<StmtPtr> body;
  };
  std::vector<Branch> branches;
  std::vector<StmtPtr> elseBody;
};

struct WhileStmt : Stmt {
  ExprPtr cond;
  std::vector<StmtPtr> body;
};

struct ForStmt : Stmt {
  std::string var;
  Span varSpan;
  ExprPtr iterable;
  std::vector<StmtPtr> body;
};

struct LoopStmt : Stmt {
  std::vector<StmtPtr> body;
};

struct ParamDecl {
  std::string name;
  Span span;
  TypeExprPtr type;  // may be null for inferred parameter types
};

struct FnDecl : Stmt {
  std::string name;
  Span nameSpan;
  std::vector<ParamDecl> params;
  TypeExprPtr returnType;  // may be null
  std::vector<StmtPtr> body;
};

struct FieldDecl {
  std::string name;
  Span span;
  TypeExprPtr type;
};

struct StructDecl : Stmt {
  std::string name;
  Span nameSpan;
  std::vector<FieldDecl> fields;
  std::vector<std::unique_ptr<FnDecl>> methods;
};

struct Program {
  std::vector<StmtPtr> statements;
};

}  // namespace a
