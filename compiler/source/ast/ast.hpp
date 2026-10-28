#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lexer/span.hpp"

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
  std::vector<std::string> qualifiers;  // module alias qualifiers: 'u' in 'u.Point'
  std::string name;
};

struct ArrayTypeExpr : TypeExpr {
  TypeExprPtr element;
};

struct OptionalTypeExpr : TypeExpr {
  TypeExprPtr element;
};

struct TupleTypeExpr : TypeExpr {
  std::vector<TypeExprPtr> elements;
};

struct FunctionTypeExpr : TypeExpr {
  std::vector<TypeExprPtr> params;
  TypeExprPtr returnType;
};

struct ParamDecl {
  std::string name;
  Span span;
  TypeExprPtr type;  // may be null for inferred parameter types
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
  ExprPtr step;   // may be null (defaults to 1)
  Span bracketSpan;
};

struct FieldExpr : Expr {
  ExprPtr object;
  std::string field;
  Span fieldSpan;
};

// `object?.field`: None propagates, Some(struct) accesses `field` and wraps it.
struct SafeNavExpr : Expr {
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
  ExprPtr step;  // may be null (defaults to 1)
};

struct TernaryExpr : Expr {
  ExprPtr cond;
  ExprPtr thenValue;
  ExprPtr elseValue;
};

struct TupleExpr : Expr {
  std::vector<ExprPtr> elements;
};

struct LambdaExpr : Expr {
  std::vector<ParamDecl> params;
  TypeExprPtr returnType;  // may be null
  ExprPtr body;
};

struct ComprehensionExpr : Expr {
  ExprPtr element;
  std::string var;
  Span varSpan;
  ExprPtr iterable;
  ExprPtr condition;  // may be null
};

struct Pattern {
  enum class Kind { Literal, Range, Wildcard };
  Kind kind = Kind::Wildcard;
  Span span;
  ExprPtr value;       // Literal patterns
  ExprPtr rangeStart;  // Range patterns (inclusive)
  ExprPtr rangeEnd;
};

struct MatchArm {
  Pattern pattern;
  ExprPtr guard;   // may be null
  ExprPtr result;
};

struct MatchExpr : Expr {
  ExprPtr subject;
  std::vector<MatchArm> arms;
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
  bool constantBinding = false;
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

struct DestructureStmt : Stmt {
  std::vector<std::string> names;  // "_" discards the slot
  std::vector<Span> nameSpans;
  bool mutableBinding = false;
  ExprPtr value;
};

struct ReturnStmt : Stmt {
  ExprPtr value;  // may be null
};

struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};

struct DeferStmt : Stmt {
  StmtPtr body;
};

// `import "lib/util.a"`, `import "lib/util.a" as u`, or `import math`.
struct ImportStmt : Stmt {
  std::string path;        // quoted path, empty for native/bare imports
  std::string alias;       // explicit alias, or empty
  std::string bareName;    // identifier for bare imports
  bool quoted = false;
  bool nativeModule = false;  // resolved to a built-in native module
  int moduleId = -1;          // resolved module id
  Span targetSpan;
};

// `from "lib/util.a" import add, Vec as Point`.
struct FromImportStmt : Stmt {
  struct Item {
    std::string name;
    std::string alias;
    Span span;
  };
  std::string path;
  std::string bareName;
  bool quoted = false;
  bool nativeModule = false;
  int moduleId = -1;
  std::vector<Item> items;
  Span targetSpan;
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

struct DoWhileStmt : Stmt {
  std::vector<StmtPtr> body;
  ExprPtr cond;
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

struct EnumVariant {
  std::string name;
  Span span;
  ExprPtr value;  // may be null (auto-incremented from the previous variant)
};

struct EnumDecl : Stmt {
  std::string name;
  Span nameSpan;
  std::vector<EnumVariant> variants;
};

struct Program {
  std::vector<StmtPtr> statements;
};

}  // namespace a
