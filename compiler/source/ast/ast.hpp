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
struct FnDecl;
struct StructDecl;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using TypeExprPtr = std::unique_ptr<TypeExpr>;

// ---------------------------------------------------------------- types

// Node tags let the backend dispatch with a single compare instead of an RTTI
// chain. Every node sets its tag in its constructor; the interpreter and any
// future bytecode compiler switch on the tag.
enum class ExprKind : std::uint8_t {
  IntLit,
  FloatLit,
  BoolLit,
  CharLit,
  StringLit,
  Ident,
  Unary,
  Binary,
  Call,
  Index,
  Slice,
  Field,
  SafeNav,
  ArrayLit,
  Range,
  Ternary,
  Tuple,
  Lambda,
  Comprehension,
  Match,
  Try,
  MapLit,
  SetLit,
};

enum class StmtKind : std::uint8_t {
  Let,
  Assign,
  Expr,
  Destructure,
  Return,
  Break,
  Continue,
  Defer,
  Import,
  FromImport,
  If,
  While,
  DoWhile,
  For,
  Loop,
  FnDecl,
  StructDecl,
  EnumDecl,
  TypeAlias,
};

// Arithmetic/logical operator identity for the backend. `op` still carries the
// source spelling for diagnostics and semantic analysis; `binop` is what the
// interpreter switches on.
enum class BinOp : std::uint8_t {
  Invalid,
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Pow,
  Eq,
  Ne,
  Lt,
  Gt,
  Le,
  Ge,
  BitAnd,
  BitOr,
  BitXor,
  Shl,
  Shr,
  And,
  Or,
  Coalesce,
  Range,
  In,
  NotIn,
};

// Statically resolved builtin identity. `Sema` marks each call site whose
// callee is not shadowed by a variable, so the interpreter can dispatch a
// builtin without re-scanning name strings on every call (the same idea as
// CPython's specialized call opcodes).
enum class BuiltinId : std::uint8_t {
  None,
  Print,
  Eprint,
  Len,
  Str,
  Sqrt,
  F64,
  I64,
  Abs,
  Round,
  Floor,
  Ceil,
  Pow,
  Join,
  Map,
  Filter,
  Sum,
  Reduce,
  Any,
  All,
  Some,
  Ok,
  Err,
  Panic,
  Assert,
  AssertEq,
};

inline BuiltinId builtinIdForName(std::string_view name) {
  if (name == "print") return BuiltinId::Print;
  if (name == "eprint") return BuiltinId::Eprint;
  if (name == "len") return BuiltinId::Len;
  if (name == "str") return BuiltinId::Str;
  if (name == "sqrt") return BuiltinId::Sqrt;
  if (name == "f64") return BuiltinId::F64;
  if (name == "i64") return BuiltinId::I64;
  if (name == "abs") return BuiltinId::Abs;
  if (name == "round") return BuiltinId::Round;
  if (name == "floor") return BuiltinId::Floor;
  if (name == "ceil") return BuiltinId::Ceil;
  if (name == "pow") return BuiltinId::Pow;
  if (name == "join") return BuiltinId::Join;
  if (name == "map") return BuiltinId::Map;
  if (name == "filter") return BuiltinId::Filter;
  if (name == "sum") return BuiltinId::Sum;
  if (name == "reduce") return BuiltinId::Reduce;
  if (name == "any") return BuiltinId::Any;
  if (name == "all") return BuiltinId::All;
  if (name == "Some") return BuiltinId::Some;
  if (name == "Ok") return BuiltinId::Ok;
  if (name == "Err") return BuiltinId::Err;
  if (name == "panic") return BuiltinId::Panic;
  if (name == "assert") return BuiltinId::Assert;
  if (name == "assert_eq") return BuiltinId::AssertEq;
  return BuiltinId::None;
}

struct TypeExpr {
  Span span;
  virtual ~TypeExpr() = default;
};

struct NamedTypeExpr : TypeExpr {
  std::vector<std::string> qualifiers;  // module alias qualifiers: 'u' in 'u.Point'
  std::string name;
  std::vector<TypeExprPtr> typeArgs;  // generic arguments: 'Foo<T>'
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
  ExprKind kind;
  explicit Expr(ExprKind k) : kind(k) {}
  virtual ~Expr() = default;
};

struct IntLitExpr : Expr {
  IntLitExpr() : Expr(ExprKind::IntLit) {}
  std::int64_t value = 0;
};

struct FloatLitExpr : Expr {
  FloatLitExpr() : Expr(ExprKind::FloatLit) {}
  double value = 0.0;
};

struct BoolLitExpr : Expr {
  BoolLitExpr() : Expr(ExprKind::BoolLit) {}
  bool value = false;
};

struct CharLitExpr : Expr {
  CharLitExpr() : Expr(ExprKind::CharLit) {}
  char32_t value = 0;
};

struct StringPart {
  bool isExpr = false;
  std::string text;  // decoded literal text
  ExprPtr expr;
};

struct StringLitExpr : Expr {
  StringLitExpr() : Expr(ExprKind::StringLit) {}
  std::vector<StringPart> parts;
};

struct IdentExpr : Expr {
  IdentExpr() : Expr(ExprKind::Ident) {}
  std::string name;
};

struct UnaryExpr : Expr {
  UnaryExpr() : Expr(ExprKind::Unary) {}
  std::string op;  // "-", "not", "!", "~", "++", or "--"
  bool postfix = false;  // true for i++ / i--, false for ++i / --i / -x / !x / ~x
  Span opSpan;
  ExprPtr operand;
};

struct BinaryExpr : Expr {
  BinaryExpr() : Expr(ExprKind::Binary) {}
  std::string op;
  BinOp binop = BinOp::Invalid;
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
  CallExpr() : Expr(ExprKind::Call) {}
  ExprPtr callee;
  std::vector<CallArg> args;

  // Resolved lazily by the interpreter. A plain identifier callee is fixed by
  // name resolution, so the classification (function, struct, or builtin) can
  // be cached per call site instead of rebuilt on every execution.
  mutable bool calleeResolved = false;
  mutable const FnDecl* calleeFunction = nullptr;
  mutable const StructDecl* calleeStruct = nullptr;

  // Set by Sema when the callee is a builtin that no local binding shadows.
  BuiltinId builtinId = BuiltinId::None;
};

struct IndexExpr : Expr {
  IndexExpr() : Expr(ExprKind::Index) {}
  ExprPtr object;
  ExprPtr index;
  Span bracketSpan;
};

struct SliceExpr : Expr {
  SliceExpr() : Expr(ExprKind::Slice) {}
  ExprPtr object;
  ExprPtr start;  // may be null
  ExprPtr end;    // may be null
  ExprPtr step;   // may be null (defaults to 1)
  Span bracketSpan;
};

struct MapLitExpr : Expr {
  MapLitExpr() : Expr(ExprKind::MapLit) {}
  std::vector<std::pair<ExprPtr, ExprPtr>> entries;  // key -> value
  Span bracketSpan;
};

struct SetLitExpr : Expr {
  SetLitExpr() : Expr(ExprKind::SetLit) {}
  std::vector<ExprPtr> elements;
  Span bracketSpan;
};

struct FieldExpr : Expr {
  FieldExpr() : Expr(ExprKind::Field) {}
  ExprPtr object;
  std::string field;
  Span fieldSpan;
};

// `object?.field`: None propagates, Some(struct) accesses `field` and wraps it.
struct SafeNavExpr : Expr {
  SafeNavExpr() : Expr(ExprKind::SafeNav) {}
  ExprPtr object;
  std::string field;
  Span fieldSpan;
};

struct ArrayLitExpr : Expr {
  ArrayLitExpr() : Expr(ExprKind::ArrayLit) {}
  std::vector<ExprPtr> elements;
};

struct RangeExpr : Expr {
  RangeExpr() : Expr(ExprKind::Range) {}
  ExprPtr start;
  ExprPtr end;
  ExprPtr step;  // may be null (defaults to 1)
};

struct TernaryExpr : Expr {
  TernaryExpr() : Expr(ExprKind::Ternary) {}
  ExprPtr cond;
  ExprPtr thenValue;
  ExprPtr elseValue;
};

struct TupleExpr : Expr {
  TupleExpr() : Expr(ExprKind::Tuple) {}
  std::vector<ExprPtr> elements;
};

struct LambdaExpr : Expr {
  LambdaExpr() : Expr(ExprKind::Lambda) {}
  std::vector<ParamDecl> params;
  TypeExprPtr returnType;  // may be null
  ExprPtr body;
};

struct ComprehensionExpr : Expr {
  ComprehensionExpr() : Expr(ExprKind::Comprehension) {}
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
  MatchExpr() : Expr(ExprKind::Match) {}
  ExprPtr subject;
  std::vector<MatchArm> arms;
};

struct TryExpr : Expr {
  TryExpr() : Expr(ExprKind::Try) {}
  ExprPtr operand;
};

// ---------------------------------------------------------------- statements

struct Stmt {
  Span span;
  StmtKind kind;
  explicit Stmt(StmtKind k) : kind(k) {}
  virtual ~Stmt() = default;
};

struct LetStmt : Stmt {
  LetStmt() : Stmt(StmtKind::Let) {}
  std::string name;
  Span nameSpan;
  TypeExprPtr type;  // may be null
  bool mutableBinding = false;
  bool constantBinding = false;
  ExprPtr init;
};

struct AssignStmt : Stmt {
  AssignStmt() : Stmt(StmtKind::Assign) {}
  std::string op;  // "=", "+=", ...
  BinOp binop = BinOp::Invalid;  // base operator for compound assignment
  Span opSpan;
  ExprPtr target;
  ExprPtr value;
};

struct ExprStmt : Stmt {
  ExprStmt() : Stmt(StmtKind::Expr) {}
  ExprPtr expr;
};

struct DestructureStmt : Stmt {
  DestructureStmt() : Stmt(StmtKind::Destructure) {}
  std::vector<std::string> names;  // "_" discards the slot
  std::vector<Span> nameSpans;
  bool mutableBinding = false;
  ExprPtr value;
};

struct ReturnStmt : Stmt {
  ReturnStmt() : Stmt(StmtKind::Return) {}
  ExprPtr value;  // may be null
};

struct BreakStmt : Stmt {
  BreakStmt() : Stmt(StmtKind::Break) {}
};
struct ContinueStmt : Stmt {
  ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct DeferStmt : Stmt {
  DeferStmt() : Stmt(StmtKind::Defer) {}
  StmtPtr body;
};

// `import "lib/util.a"`, `import "lib/util.a" as u`, or `import math`.
struct ImportStmt : Stmt {
  ImportStmt() : Stmt(StmtKind::Import) {}
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
  FromImportStmt() : Stmt(StmtKind::FromImport) {}
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
  IfStmt() : Stmt(StmtKind::If) {}
  struct Branch {
    ExprPtr cond;
    std::vector<StmtPtr> body;
  };
  std::vector<Branch> branches;
  std::vector<StmtPtr> elseBody;
};

struct WhileStmt : Stmt {
  WhileStmt() : Stmt(StmtKind::While) {}
  ExprPtr cond;
  std::vector<StmtPtr> body;
};

struct DoWhileStmt : Stmt {
  DoWhileStmt() : Stmt(StmtKind::DoWhile) {}
  std::vector<StmtPtr> body;
  ExprPtr cond;
};

struct ForStmt : Stmt {
  ForStmt() : Stmt(StmtKind::For) {}
  std::string var;
  Span varSpan;
  ExprPtr iterable;
  std::vector<StmtPtr> body;
};

struct LoopStmt : Stmt {
  LoopStmt() : Stmt(StmtKind::Loop) {}
  std::vector<StmtPtr> body;
};

struct FnDecl : Stmt {
  FnDecl() : Stmt(StmtKind::FnDecl) {}
  std::string name;
  Span nameSpan;
  std::vector<std::string> typeParams;  // generic type parameters
  std::vector<ParamDecl> params;
  TypeExprPtr returnType;  // may be null
  std::vector<StmtPtr> body;
  int moduleId = -1;  // module owning the function, filled in by Sema
};

struct FieldDecl {
  std::string name;
  Span span;
  TypeExprPtr type;
};

struct StructDecl : Stmt {
  StructDecl() : Stmt(StmtKind::StructDecl) {}
  std::string name;
  Span nameSpan;
  std::vector<std::string> typeParams;  // generic type parameters
  std::string baseClass;  // base class name if class(Base)
  std::string metaclass;  // metaclass name if specified
  std::vector<FieldDecl> fields;
  std::vector<std::unique_ptr<FnDecl>> methods;
  bool isClass = false;
};

struct EnumVariant {
  std::string name;
  Span span;
  ExprPtr value;  // may be null (auto-incremented from the previous variant)
};

struct EnumDecl : Stmt {
  EnumDecl() : Stmt(StmtKind::EnumDecl) {}
  std::string name;
  Span nameSpan;
  std::vector<EnumVariant> variants;
};

struct TypeAliasDecl : Stmt {
  TypeAliasDecl() : Stmt(StmtKind::TypeAlias) {}
  std::string name;
  Span nameSpan;
  TypeExprPtr type;
};

struct Program {
  std::vector<StmtPtr> statements;
};

}  // namespace a
