#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ast/ast.hpp"
#include "lexer/span.hpp"
#include "type-checker/types.hpp"

namespace a {

struct ModuleGraph;
struct SemaResult;

namespace ir {

// Typed SSA instructions. The lowering covers the core statement/expression
// subset with real control-flow blocks and phi nodes; constructs that cannot be
// represented yet (closures, pattern matching, comprehensions) become Opaque.
enum class Op {
  ConstInt,
  ConstFloat,
  ConstBool,
  ConstChar,
  ConstString,
  Param,
  Phi,
  Copy,
  Binary,
  Unary,
  Select,
  Call,
  CallValue,
  Index,
  Slice,
  Field,
  Array,
  Tuple,
  Map,
  Set,
  Range,
  StringConcat,
  IterInit,
  IterHasNext,
  IterNext,
  TupleGet,
  StoreIndex,
  StoreField,
  Global,
  FuncRef,
  Lambda,
  Opaque,
};

const char* opName(Op op);

struct Inst {
  Op op = Op::Opaque;
  int id = -1;  // SSA value id; assigned per function
  TypePtr type;
  std::string text;  // operator spelling, callee/method name, or field name
  std::int64_t intValue = 0;
  double floatValue = 0.0;
  bool boolValue = false;
  char32_t charValue = 0;
  std::string stringValue;
  std::vector<int> operands;
  std::vector<int> incomingBlocks;  // Phi only, parallel to `operands`
  Span span;

  bool producesValue() const { return id >= 0; }
};

struct Terminator {
  enum class Kind { Jump, Branch, Return, Unreachable };
  Kind kind = Kind::Unreachable;
  int target = -1;
  int falseTarget = -1;
  int value = -1;
  Span span;
};

struct Block {
  std::string label;
  std::vector<Inst> insts;
  Terminator term;
};

struct Parameter {
  std::string name;
  TypePtr type;
  int value = -1;
};

struct Function {
  std::string name;
  const FnDecl* decl = nullptr;
  std::vector<Parameter> params;
  TypePtr returnType;
  std::vector<Block> blocks;
  int nextValue = 0;
  bool isEntry = false;
};

struct Program {
  std::vector<Function> functions;
};

// Lowers every top-level function in the module graph plus the entry module's
// top-level statements (as `main`). Requires a successfully analyzed program.
Program lowerProgram(const ModuleGraph& graph, const SemaResult& sema);

std::string printFunction(const Function& function);
std::string printProgram(const Program& program);

// Resolves a variable type and applies the default for unconstrained literal
// variables (integer literals default to i64, float literals to f64).
TypePtr defaultType(const TypePtr& type);

}  // namespace ir
}  // namespace a
