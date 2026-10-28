#include "backend/interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <random>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "errors/runtime_error.hpp"
#include "io/file_io.hpp"
#include "name-resolution/native_modules.hpp"
#include "strings/format.hpp"
#include "strings/utf8.hpp"

namespace a {
namespace {

constexpr int kMaxCallDepth = 2000;

// Tagged casts. Each AST node carries its kind; a matching kind means the
// static_cast is valid and costs one compare instead of an RTTI lookup.
template <class T>
const T* asExpr(const Expr& expr, ExprKind kind) {
  return expr.kind == kind ? static_cast<const T*>(&expr) : nullptr;
}

template <class T>
const T* asStmt(const Stmt& stmt, StmtKind kind) {
  return stmt.kind == kind ? static_cast<const T*>(&stmt) : nullptr;
}

const char* binOpName(BinOp op) {
  switch (op) {
    case BinOp::Add: return "+";
    case BinOp::Sub: return "-";
    case BinOp::Mul: return "*";
    case BinOp::Div: return "/";
    case BinOp::Mod: return "%";
    case BinOp::Pow: return "**";
    case BinOp::Eq: return "==";
    case BinOp::Ne: return "!=";
    case BinOp::Lt: return "<";
    case BinOp::Gt: return ">";
    case BinOp::Le: return "<=";
    case BinOp::Ge: return ">=";
    case BinOp::BitAnd: return "&";
    case BinOp::BitOr: return "|";
    case BinOp::BitXor: return "^";
    case BinOp::Shl: return "<<";
    case BinOp::Shr: return ">>";
    case BinOp::And: return "and";
    case BinOp::Or: return "or";
    case BinOp::Coalesce: return "??";
    case BinOp::Range: return "..";
    case BinOp::In: return "in";
    case BinOp::NotIn: return "not in";
    case BinOp::Invalid: break;
  }
  return "?";
}

struct CallDepthGuard {
  int& depth;
  explicit CallDepthGuard(int& d) : depth(d) { ++depth; }
  ~CallDepthGuard() { --depth; }
};

Value makeVoid() { return Value{}; }

Value makeBool(bool value) {
  Value out;
  out.data = value;
  return out;
}

Value makeInt(std::int64_t value) {
  Value out;
  out.data = value;
  return out;
}

Value makeFloat(double value) {
  Value out;
  out.data = value;
  return out;
}

Value makeChar(char32_t value) {
  Value out;
  out.data = value;
  return out;
}

Value makeString(std::string value) {
  Value out;
  out.data = std::move(value);
  return out;
}

Value makeArray(ArrayPtr value) {
  Value out;
  out.data = std::move(value);
  return out;
}

Value makeStructValue(StructPtr value) {
  Value out;
  out.data = std::move(value);
  return out;
}

Value makeRange(std::int64_t start, std::int64_t end, std::int64_t step = 1) {
  Value out;
  out.data = RangeValue{start, end, step};
  return out;
}

Value makeEnum(std::string name) {
  auto object = std::make_shared<EnumObj>();
  object->name = std::move(name);
  Value out;
  out.data = std::move(object);
  return out;
}

Value makeOptional(bool hasValue, Value value) {
  auto optional = std::make_shared<OptionalValue>();
  optional->hasValue = hasValue;
  optional->value = std::move(value);
  auto out = Value{};
  out.data = std::move(optional);
  return out;
}

Value makeResult(bool ok, Value value) {
  auto result = std::make_shared<ResultValue>();
  result->ok = ok;
  result->value = std::move(value);
  auto out = Value{};
  out.data = std::move(result);
  return out;
}

Value makeNone() { return makeOptional(false, Value{}); }

Value makeTuple(std::vector<Value> items) {
  auto tuple = std::make_shared<TupleValue>();
  tuple->items = std::move(items);
  auto out = Value{};
  out.data = std::move(tuple);
  return out;
}

Value makeClosureValue(ClosurePtr closure) {
  Value out;
  out.data = std::move(closure);
  return out;
}

auto& randomEngine() {
  static std::mt19937_64 engine{std::random_device{}()};
  return engine;
}

bool builtinConstantValue(const std::string& name, Value& out) {
  if (name == "PI") {
    out = makeFloat(3.14159265358979323846);
    return true;
  }
  if (name == "E") {
    out = makeFloat(2.71828182845904523536);
    return true;
  }
  struct IntConstant {
    const char* name;
    std::int64_t value;
  };
  static const IntConstant ints[] = {
      {"I8_MIN", std::numeric_limits<std::int8_t>::min()},
      {"I8_MAX", std::numeric_limits<std::int8_t>::max()},
      {"I16_MIN", std::numeric_limits<std::int16_t>::min()},
      {"I16_MAX", std::numeric_limits<std::int16_t>::max()},
      {"I32_MIN", std::numeric_limits<std::int32_t>::min()},
      {"I32_MAX", std::numeric_limits<std::int32_t>::max()},
      {"I64_MIN", std::numeric_limits<std::int64_t>::min()},
      {"I64_MAX", std::numeric_limits<std::int64_t>::max()},
      {"U8_MAX", std::numeric_limits<std::uint8_t>::max()},
      {"U16_MAX", std::numeric_limits<std::uint16_t>::max()},
      {"U32_MAX", std::numeric_limits<std::uint32_t>::max()}};
  for (const auto& entry : ints) {
    if (name == entry.name) {
      out = makeInt(entry.value);
      return true;
    }
  }
  if (name == "F32_MIN") {
    out = makeFloat(-static_cast<double>(std::numeric_limits<float>::max()));
    return true;
  }
  if (name == "F32_MAX") {
    out = makeFloat(static_cast<double>(std::numeric_limits<float>::max()));
    return true;
  }
  if (name == "F64_MIN") {
    out = makeFloat(-std::numeric_limits<double>::max());
    return true;
  }
  if (name == "F64_MAX") {
    out = makeFloat(std::numeric_limits<double>::max());
    return true;
  }
  return false;
}

bool parseInteger(const std::string& text, std::int64_t& out) {
  if (text.empty()) return false;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc() && result.ptr == end;
}

bool parseFloat(const std::string& text, double& out) {
  if (text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double value = std::strtod(text.c_str(), &end);
  if (errno == ERANGE) return false;
  if (end != text.c_str() + text.size()) return false;
  out = value;
  return true;
}

}  // namespace

Interpreter::Interpreter(const ModuleGraph& graph, const SemaResult& result,
                         DiagnosticSink& sink, std::istream& in, std::ostream& out,
                         std::ostream& err)
    : graph_(graph), sema_(result), sink_(sink), in_(in), out_(out), err_(err) {}

void Interpreter::fail(Span span, std::string message, std::string help) {
  throw RuntimeError{span, std::move(message), std::move(help)};
}

Interpreter::Env& Interpreter::moduleEnv(int moduleId) {
  return moduleEnvs_[static_cast<std::size_t>(moduleId)];
}

std::string Interpreter::qualifiedName(int moduleId, const std::string& name) const {
  return graph_.at(moduleId).name + "::" + name;
}

std::string Interpreter::qualifiedName(const std::string& name) const {
  return qualifiedName(currentModule_, name);
}

std::string Interpreter::shortStructName(const std::string& name) const {
  std::size_t pos = name.rfind("::");
  return pos == std::string::npos ? name : name.substr(pos + 2);
}

int Interpreter::run() {
  moduleEnvs_.resize(graph_.modules.size());
  try {
    // Modules execute once, dependencies first; the entry module runs last.
    for (int id : graph_.order) {
      Env& env = moduleEnv(id);
      env.parent = nullptr;
      currentModule_ = id;
      execStatements(graph_.at(id).program->statements, env);
    }
    return 0;
  } catch (RuntimeError& error) {
    sink_.error(error.span, std::move(error.message), std::move(error.help));
    return 2;
  } catch (ReturnSignal&) {
    sink_.error(Span{}, "Internal error: 'return' escaped the top level.");
    return 2;
  } catch (BreakSignal&) {
    sink_.error(Span{}, "Internal error: 'break' escaped a loop.");
    return 2;
  } catch (ContinueSignal&) {
    sink_.error(Span{}, "Internal error: 'continue' escaped a loop.");
    return 2;
  } catch (std::exception& error) {
    sink_.error(Span{}, std::string("Internal error: ") + error.what());
    return 2;
  }
}

// ---------------------------------------------------------------- execution

void Interpreter::execStatements(const std::vector<StmtPtr>& statements, Env& env) {
  std::vector<const Stmt*> defers;
  std::exception_ptr pending;

  try {
    for (const auto& statement : statements) {
      if (auto* defer = asStmt<DeferStmt>(*statement, StmtKind::Defer)) {
        defers.push_back(defer->body.get());
        continue;
      }
      exec(*statement, env);
      if (returning_) break;
    }
  } catch (...) {
    pending = std::current_exception();
  }

  for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
    if (pending) {
      try {
        exec(**it, env);
      } catch (...) {
        // Preserve the original exception.
      }
    } else {
      try {
        exec(**it, env);
      } catch (...) {
        pending = std::current_exception();
      }
    }
  }

  if (pending) std::rethrow_exception(pending);
}

void Interpreter::execBlock(const std::vector<StmtPtr>& body, Env& parent) {
  Env child;
  child.parent = &parent;
  execStatements(body, child);
}

void Interpreter::exec(const Stmt& stmt, Env& env) {
  switch (stmt.kind) {
  case StmtKind::Let: {
    auto* node = static_cast<const LetStmt*>(&stmt);
    Binding binding;
    binding.value = eval(*node->init, env);
    binding.mutableBinding = node->mutableBinding;
    env.vars[node->name] = std::move(binding);
    return;
  }

  case StmtKind::Assign: {
    auto* node = static_cast<const AssignStmt*>(&stmt);

    // `name += expr` is the hot form in loops. Skip building an LValue (which
    // carries a variant and a shared_ptr) and update the binding in place.
    if (node->binop != BinOp::Invalid) {
      if (auto* ident = asExpr<IdentExpr>(*node->target, ExprKind::Ident)) {
        if (Binding* binding = env.find(ident->name)) {
          if (node->binop == BinOp::Add) {
            if (auto* current = std::get_if<std::int64_t>(&binding->value.data)) {
              Value right = eval(*node->value, env);
              if (auto* addend = std::get_if<std::int64_t>(&right.data)) {
                std::int64_t sum = 0;
                if (__builtin_add_overflow(*current, *addend, &sum)) {
                  fail(node->opSpan, "Integer overflow in '+'.");
                }
                binding->value.data = sum;
                return;
              }
              binding->value = applyBinary(node->binop, binding->value, right, node->opSpan);
              return;
            }
          }
          binding->value =
              applyBinary(node->binop, binding->value, eval(*node->value, env), node->opSpan);
          return;
        }
      }
    }

    LValue target = resolveLValue(*node->target, env);
    if (node->op == "=") {
      writeLValue(target, eval(*node->value, env));
    } else {
      Value left = readLValue(target);
      Value right = eval(*node->value, env);
      writeLValue(target, applyBinary(node->binop, left, right, node->opSpan));
    }
    return;
  }

  case StmtKind::Destructure: {
    auto* node = static_cast<const DestructureStmt*>(&stmt);
    Value value = eval(*node->value, env);
    auto* tuple = std::get_if<TuplePtr>(&value.data);
    if (!tuple) fail(node->span, "Destructuring requires a tuple value.");
    if ((*tuple)->items.size() != node->names.size()) {
      fail(node->span, "This tuple has " + std::to_string((*tuple)->items.size()) +
                           " value(s), but the pattern binds " +
                           std::to_string(node->names.size()) + ".");
    }
    for (std::size_t i = 0; i < node->names.size(); ++i) {
      if (node->names[i] == "_") continue;
      Binding binding;
      binding.value = (*tuple)->items[i];
      binding.mutableBinding = node->mutableBinding;
      env.vars[node->names[i]] = std::move(binding);
    }
    return;
  }

  case StmtKind::Expr: {
    auto* node = static_cast<const ExprStmt*>(&stmt);
    eval(*node->expr, env);
    return;
  }

  case StmtKind::If: {
    auto* node = static_cast<const IfStmt*>(&stmt);
    for (auto& branch : node->branches) {
      Value condition = eval(*branch.cond, env);
      if (expectBool(condition, branch.cond->span, "The 'if' condition")) {
        execBlock(branch.body, env);
        return;
      }
    }
    if (!node->elseBody.empty()) execBlock(node->elseBody, env);
    return;
  }

  case StmtKind::While: {
    auto* node = static_cast<const WhileStmt*>(&stmt);
    while (true) {
      Value condition = eval(*node->cond, env);
      if (!expectBool(condition, node->cond->span, "The 'while' condition")) break;
      try {
        execBlock(node->body, env);
      } catch (BreakSignal&) {
        break;
      } catch (ContinueSignal&) {
        continue;
      }
      if (returning_) return;
    }
    return;
  }

  case StmtKind::DoWhile: {
    auto* node = static_cast<const DoWhileStmt*>(&stmt);
    while (true) {
      try {
        execBlock(node->body, env);
      } catch (BreakSignal&) {
        break;
      } catch (ContinueSignal&) {
        // fall through to the condition
      }
      if (returning_) return;
      Value condition = eval(*node->cond, env);
      if (!expectBool(condition, node->cond->span, "The 'while' condition")) break;
    }
    return;
  }

  case StmtKind::For: {
    auto* node = static_cast<const ForStmt*>(&stmt);
    Value iterable = eval(*node->iterable, env);

    // One scope for the whole loop. The loop variable binding is created once
    // and updated in place; the body no longer allocates a hash table per
    // iteration. Declarations in the body are re-initialized each pass.
    Env child;
    child.parent = &env;
    child.vars[node->var].mutableBinding = false;

    auto runIteration = [&](Value element) {
      child.vars[node->var].value = std::move(element);
      execStatements(node->body, child);
    };

    if (auto* range = std::get_if<RangeValue>(&iterable.data)) {
      for (std::int64_t i = range->start; i < range->end; i += range->step) {
        try {
          runIteration(makeInt(i));
        } catch (BreakSignal&) {
          break;
        } catch (ContinueSignal&) {
          // next iteration
        }
        if (returning_) return;
        if (i > std::numeric_limits<std::int64_t>::max() - range->step) break;
      }
      return;
    }

    if (auto* array = std::get_if<ArrayPtr>(&iterable.data)) {
      const auto snapshot = **array;  // value snapshot: safe if the body mutates the array
      for (const auto& element : snapshot) {
        try {
          runIteration(element);
        } catch (BreakSignal&) {
          break;
        } catch (ContinueSignal&) {
          // next iteration
        }
        if (returning_) return;
      }
      return;
    }

    if (auto* text = std::get_if<std::string>(&iterable.data)) {
      std::size_t index = 0;
      char32_t codepoint = 0;
      while (decodeUtf8(*text, index, codepoint)) {
        try {
          runIteration(makeChar(codepoint));
        } catch (BreakSignal&) {
          break;
        } catch (ContinueSignal&) {
          // next iteration
        }
        if (returning_) return;
      }
      return;
    }

    fail(node->iterable->span, "This value cannot be iterated over.");
  }

  case StmtKind::Loop: {
    auto* node = static_cast<const LoopStmt*>(&stmt);
    while (true) {
      try {
        execBlock(node->body, env);
      } catch (BreakSignal&) {
        break;
      } catch (ContinueSignal&) {
        continue;
      }
      if (returning_) return;
    }
    return;
  }

  case StmtKind::Return: {
    auto* node = static_cast<const ReturnStmt*>(&stmt);
    returnValue_ = node->value ? eval(*node->value, env) : makeVoid();
    returning_ = true;
    return;
  }

  case StmtKind::Break: throw BreakSignal{};
  case StmtKind::Continue: throw ContinueSignal{};

  case StmtKind::Defer: {
    auto* node = static_cast<const DeferStmt*>(&stmt);
    exec(*node->body, env);
    return;
  }

  case StmtKind::Import: {
    auto* node = static_cast<const ImportStmt*>(&stmt);
    bindImport(*node, env);
    return;
  }

  case StmtKind::FromImport: {
    auto* node = static_cast<const FromImportStmt*>(&stmt);
    bindFromImport(*node, env);
    return;
  }

  case StmtKind::FnDecl:
  case StmtKind::StructDecl:
  case StmtKind::EnumDecl:
  case StmtKind::TypeAlias:
    // Declarations have no runtime effect; they are registered by sema.
    return;
  }
}

void Interpreter::bindImport(const ImportStmt& stmt, Env& env) {
  std::string alias = stmt.alias;
  if (alias.empty()) {
    if (stmt.quoted) {
      std::size_t slash = stmt.path.find_last_of("/\\");
      std::string stem = slash == std::string::npos ? stmt.path : stmt.path.substr(slash + 1);
      std::size_t dot = stem.find_last_of('.');
      if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
      alias = std::move(stem);
    } else {
      alias = stmt.bareName;
    }
  }
  if (alias.empty()) return;

  auto module = std::make_shared<ModuleObj>();
  if (stmt.nativeModule) {
    module->nativeName = stmt.bareName;
  } else {
    if (stmt.moduleId < 0) return;  // unresolved; the compiler already reported it
    module->id = stmt.moduleId;
  }

  Binding binding;
  binding.value.data = ModulePtr(std::move(module));
  binding.mutableBinding = false;
  env.vars[alias] = std::move(binding);
}

void Interpreter::bindFromImport(const FromImportStmt& stmt, Env& env) {
  for (const auto& item : stmt.items) {
    Value value;
    if (stmt.nativeModule) {
      if (const std::string* fn = nativeFunction(stmt.bareName, item.name)) {
        auto native = std::make_shared<NativeFunction>();
        native->builtinName = *fn;
        value.data = NativeFunctionPtr(std::move(native));
      } else if (const std::string* constant = nativeConstant(stmt.bareName, item.name)) {
        if (!builtinConstantValue(*constant, value)) continue;
      } else {
        continue;
      }
    } else {
      if (stmt.moduleId < 0) continue;
      const ModuleExports& exports = sema_.modules[static_cast<std::size_t>(stmt.moduleId)];
      auto fn = exports.functions.find(item.name);
      if (fn != exports.functions.end()) {
        auto closure = std::make_shared<Closure>();
        closure->fn = fn->second;
        closure->moduleId = stmt.moduleId;
        value.data = ClosurePtr(std::move(closure));
      } else if (exports.values.count(item.name)) {
        Binding* binding = moduleEnv(stmt.moduleId).find(item.name);
        if (!binding) continue;
        value = binding->value;
      } else {
        continue;  // struct imports bind type names only
      }
    }

    Binding binding;
    binding.value = std::move(value);
    binding.mutableBinding = false;
    env.vars[item.alias] = std::move(binding);
  }
}

// ---------------------------------------------------------------- lvalues

Interpreter::LValue Interpreter::resolveLValue(const Expr& target, Env& env) {
  LValue lvalue;

  if (auto* ident = asExpr<IdentExpr>(target, ExprKind::Ident)) {
    Binding* binding = env.find(ident->name);
    if (!binding) {
      Binding fresh;
      fresh.mutableBinding = true;
      env.vars[ident->name] = std::move(fresh);
      binding = &env.vars[ident->name];
    }
    lvalue.kind = LValue::Kind::Variable;
    lvalue.binding = binding;
    return lvalue;
  }

  if (auto* field = asExpr<FieldExpr>(target, ExprKind::Field)) {
    Value objectValue = eval(*field->object, env);
    if (auto* modulePtr = std::get_if<ModulePtr>(&objectValue.data)) {
      const ModuleObj& module = **modulePtr;
      if (module.id < 0) fail(field->span, "Cannot assign to a member of a native module.");
      Binding* binding = moduleBinding(module, field->field);
      if (!binding) {
        fail(field->fieldSpan, "Module has no variable '" + field->field +
                                   "' to assign to.");
      }
      if (!binding->mutableBinding) {
        fail(field->fieldSpan,
             "Cannot assign to immutable module variable '" + field->field + "'.",
             "Declare it with 'var' to make it mutable.");
      }
      lvalue.kind = LValue::Kind::Variable;
      lvalue.binding = binding;
      return lvalue;
    }
    auto* object = std::get_if<StructPtr>(&objectValue.data);
    if (!object) fail(field->span, "Field assignment requires a struct value.");
    lvalue.kind = LValue::Kind::Field;
    lvalue.object = *object;
    for (std::size_t i = 0; i < lvalue.object->fields.size(); ++i) {
      if (lvalue.object->fields[i].first == field->field) {
        lvalue.fieldIndex = i;
        return lvalue;
      }
    }
    fail(field->fieldSpan, "Struct '" + shortStructName(lvalue.object->typeName) +
                               "' has no field '" + field->field + "'.");
  }

  if (auto* index = asExpr<IndexExpr>(target, ExprKind::Index)) {
    Value objectValue = eval(*index->object, env);
    auto* array = std::get_if<ArrayPtr>(&objectValue.data);
    if (!array) fail(index->span, "Index assignment requires an array.");
    Value indexValue = eval(*index->index, env);
    std::int64_t raw = expectInt(indexValue, index->index->span, "The array index");
    lvalue.kind = LValue::Kind::Element;
    lvalue.array = *array;
    lvalue.index = static_cast<std::size_t>(normalizeIndex(raw, lvalue.array->size(), index->span));
    return lvalue;
  }

  fail(target.span, "Invalid assignment target.");
}

Value Interpreter::readLValue(const LValue& lvalue) {
  switch (lvalue.kind) {
    case LValue::Kind::Variable:
      return lvalue.binding->value;
    case LValue::Kind::Field:
      return lvalue.object->fields[lvalue.fieldIndex].second;
    case LValue::Kind::Element:
      return (*lvalue.array)[lvalue.index];
  }
  return makeVoid();
}

void Interpreter::writeLValue(const LValue& lvalue, Value value) {
  switch (lvalue.kind) {
    case LValue::Kind::Variable:
      lvalue.binding->value = std::move(value);
      return;
    case LValue::Kind::Field:
      lvalue.object->fields[lvalue.fieldIndex].second = std::move(value);
      return;
    case LValue::Kind::Element:
      (*lvalue.array)[lvalue.index] = std::move(value);
      return;
  }
}

std::int64_t Interpreter::normalizeIndex(std::int64_t index, std::size_t length, Span span) {
  if (index < 0) {
    index = static_cast<std::int64_t>(length) + index;
  }
  if (index < 0 || static_cast<std::size_t>(index) >= length) {
    fail(span, "Array index " + std::to_string(index) + " is out of bounds for an array of length " +
                   std::to_string(length) + ".");
  }
  return index;
}

// ---------------------------------------------------------------- evaluation

Value Interpreter::eval(const Expr& expr, Env& env) {
  switch (expr.kind) {
  case ExprKind::IntLit: return makeInt(static_cast<const IntLitExpr&>(expr).value);
  case ExprKind::FloatLit: return makeFloat(static_cast<const FloatLitExpr&>(expr).value);
  case ExprKind::BoolLit: return makeBool(static_cast<const BoolLitExpr&>(expr).value);
  case ExprKind::CharLit: return makeChar(static_cast<const CharLitExpr&>(expr).value);
  case ExprKind::StringLit: return evalString(static_cast<const StringLitExpr&>(expr), env);

  case ExprKind::Ident: return evalIdentifier(static_cast<const IdentExpr&>(expr), env);

  case ExprKind::Unary: {
    auto* node = static_cast<const UnaryExpr*>(&expr);
    Value operand = eval(*node->operand, env);
    if (node->op == "!") {      auto* optional = std::get_if<OptionalPtr>(&operand.data);
      if (!optional) fail(node->span, "Operator '!' requires an optional value.");
      if (!(*optional)->hasValue) {
        fail(node->span, "Cannot unwrap None with '!'.",
             "Check for None with '\?\?' or compare with 'None' first.");
      }
      return (*optional)->value;
    }
    if (node->op == "not") {
      return makeBool(!expectBool(operand, node->span, "The 'not' operand"));
    }
    if (node->op == "~") {
      auto* value = std::get_if<std::int64_t>(&operand.data);
      if (!value) fail(node->span, "Operator '~' requires an integer operand.");
      return makeInt(~*value);
    }
    if (auto* value = std::get_if<std::int64_t>(&operand.data)) {
      if (*value == std::numeric_limits<std::int64_t>::min()) {
        fail(node->span, "Integer overflow while negating the smallest integer.");
      }
      return makeInt(-*value);
    }
    if (auto* value = std::get_if<double>(&operand.data)) return makeFloat(-*value);
    fail(node->span, "Operator '-' requires a numeric operand.");
  }

  case ExprKind::Binary: return evalBinary(static_cast<const BinaryExpr&>(expr), env);
  case ExprKind::Range: {
    auto* node = static_cast<const RangeExpr*>(&expr);
    std::int64_t start = expectInt(eval(*node->start, env), node->start->span, "The range start");
    std::int64_t end = expectInt(eval(*node->end, env), node->end->span, "The range end");
    std::int64_t step = 1;
    if (node->step) {
      step = expectInt(eval(*node->step, env), node->step->span, "The range step");
      if (step <= 0) {
        fail(node->step->span,
             "The range step must be positive, found " + std::to_string(step) + ".",
             "Write start..end..step, e.g. 0..10..2.");
      }
    }
    return makeRange(start, end, step);
  }
  case ExprKind::Ternary: {
    auto* node = static_cast<const TernaryExpr*>(&expr);
    if (expectBool(eval(*node->cond, env), node->cond->span, "The condition of '\?:'")) {
      return eval(*node->thenValue, env);
    }
    return eval(*node->elseValue, env);
  }

  case ExprKind::Call: return evalCall(static_cast<const CallExpr&>(expr), env);
  case ExprKind::Index: return evalIndex(static_cast<const IndexExpr&>(expr), env);
  case ExprKind::Slice: return evalSlice(static_cast<const SliceExpr&>(expr), env);
  case ExprKind::Field: return evalField(static_cast<const FieldExpr&>(expr), env);
  case ExprKind::SafeNav: return evalSafeNav(static_cast<const SafeNavExpr&>(expr), env);
  case ExprKind::ArrayLit: return evalArray(static_cast<const ArrayLitExpr&>(expr), env);
  case ExprKind::Tuple: return evalTuple(static_cast<const TupleExpr&>(expr), env);
  case ExprKind::Lambda: return evalLambda(static_cast<const LambdaExpr&>(expr), env);
  case ExprKind::Match: return evalMatch(static_cast<const MatchExpr&>(expr), env);
  case ExprKind::Try: return evalTry(static_cast<const TryExpr&>(expr), env);
  case ExprKind::Comprehension: {
    auto* node = static_cast<const ComprehensionExpr*>(&expr);
    return evalComprehension(*node, env);
  }
  }

  fail(expr.span, "Internal error: unsupported expression.");
}

Value Interpreter::evalIdentifier(const IdentExpr& expr, Env& env) {
  if (Binding* binding = env.find(expr.name)) return binding->value;
  if (expr.name == "None") return makeNone();
  Value constant;
  if (builtinConstantValue(expr.name, constant)) return constant;
  const std::string enumQ = qualifiedName(expr.name);
  if (sema_.enums.count(enumQ)) return makeEnum(enumQ);
  auto fnIt = sema_.functions.find(qualifiedName(expr.name));
  if (fnIt != sema_.functions.end()) {
    auto closure = std::make_shared<Closure>();
    closure->fn = fnIt->second;
    closure->moduleId = currentModule_;
    return makeClosureValue(std::move(closure));
  }
  fail(expr.span, "Undefined variable '" + expr.name + "'.");
}

Value Interpreter::evalString(const StringLitExpr& expr, Env& env) {
  std::string out;
  for (const auto& part : expr.parts) {
    if (part.isExpr) {
      if (!part.expr) continue;
      out += formatValue(eval(*part.expr, env), false);
    } else {
      out += part.text;
    }
  }
  return makeString(std::move(out));
}

Value Interpreter::evalArray(const ArrayLitExpr& expr, Env& env) {
  auto array = std::make_shared<std::vector<Value>>();
  array->reserve(expr.elements.size());
  for (const auto& element : expr.elements) {
    array->push_back(eval(*element, env));
  }
  return makeArray(std::move(array));
}

Value Interpreter::evalIndex(const IndexExpr& expr, Env& env) {
  Value object = eval(*expr.object, env);
  Value indexValue = eval(*expr.index, env);
  std::int64_t index = expectInt(indexValue, expr.index->span, "The index");

  if (auto* array = std::get_if<ArrayPtr>(&object.data)) {
    std::size_t normalized =
        static_cast<std::size_t>(normalizeIndex(index, (*array)->size(), expr.span));
    return (**array)[normalized];
  }
  if (auto* text = std::get_if<std::string>(&object.data)) {
    std::size_t length = codepointCount(*text);
    std::size_t normalized = static_cast<std::size_t>(normalizeIndex(index, length, expr.span));
    return makeChar(codepointAt(*text, normalized));
  }
  fail(expr.span, "Only arrays and strings can be indexed.");
}

Value Interpreter::evalSlice(const SliceExpr& expr, Env& env) {
  Value object = eval(*expr.object, env);
  bool isString = false;
  std::int64_t length = 0;
  if (auto* array = std::get_if<ArrayPtr>(&object.data)) {
    length = static_cast<std::int64_t>((*array)->size());
  } else if (auto* text = std::get_if<std::string>(&object.data)) {
    isString = true;
    length = static_cast<std::int64_t>(codepointCount(*text));
  } else {
    fail(expr.span, "Only arrays and strings can be sliced.");
  }

  std::int64_t start = 0;
  std::int64_t end = length;
  std::int64_t step = 1;
  if (expr.start) {
    start = expectInt(eval(*expr.start, env), expr.start->span, "The slice start");
    if (start < 0) start = length + start;
    if (start < 0) start = 0;
  }
  if (expr.end) {
    end = expectInt(eval(*expr.end, env), expr.end->span, "The slice end");
    if (end < 0) end = length + end;
    if (end < 0) end = 0;
  }
  if (expr.step) {
    step = expectInt(eval(*expr.step, env), expr.step->span, "The slice step");
    if (step <= 0) {
      fail(expr.step->span, "The slice step must be positive, found " + std::to_string(step) + ".",
           "Stepped slices walk forward only, e.g. items[0..10..2].");
    }
  }
  if (start > length) start = length;
  if (end > length) end = length;
  if (start > end) start = end;

  if (isString) {
    const std::string& text = std::get<std::string>(object.data);
    std::string out;
    for (std::int64_t i = start; i < end; i += step) {
      out += utf8Encode(codepointAt(text, static_cast<std::size_t>(i)));
    }
    return makeString(std::move(out));
  }

  auto* array = std::get_if<ArrayPtr>(&object.data);
  auto result = std::make_shared<std::vector<Value>>();
  for (std::int64_t i = start; i < end; i += step) {
    result->push_back((**array)[static_cast<std::size_t>(i)]);
  }
  return makeArray(std::move(result));
}

Value Interpreter::evalField(const FieldExpr& expr, Env& env) {
  Value object = eval(*expr.object, env);

  if (auto* enumPtr = std::get_if<EnumPtr>(&object.data)) {
    auto info = sema_.enums.find((*enumPtr)->name);
    if (info != sema_.enums.end()) {
      auto value = info->second.values.find(expr.field);
      if (value != info->second.values.end()) return makeInt(value->second);
    }
    fail(expr.fieldSpan,
         "Enum '" + shortStructName((*enumPtr)->name) + "' has no variant named '" + expr.field +
             "'.");
  }

  if (auto* modulePtr = std::get_if<ModulePtr>(&object.data)) {
    const ModuleObj& module = **modulePtr;
    if (module.id < 0) {
      Value result;
      if (const std::string* fn = nativeFunction(module.nativeName, expr.field)) {
        auto native = std::make_shared<NativeFunction>();
        native->builtinName = *fn;
        result.data = NativeFunctionPtr(std::move(native));
        return result;
      }
      if (const std::string* name = nativeConstant(module.nativeName, expr.field)) {
        if (builtinConstantValue(*name, result)) return result;
      }
      fail(expr.fieldSpan,
           "Module '" + module.nativeName + "' has no member '" + expr.field + "'.");
    }

    const ModuleExports& exports = sema_.modules[static_cast<std::size_t>(module.id)];
    if (auto fn = exports.functions.find(expr.field); fn != exports.functions.end()) {
      auto closure = std::make_shared<Closure>();
      closure->fn = fn->second;
      closure->moduleId = module.id;
      return makeClosureValue(std::move(closure));
    }
    if (exports.structs.count(expr.field)) {
      fail(expr.fieldSpan,
           "'" + exports.name + "." + expr.field + "' is a struct type, not a value.",
           "Construct it: " + exports.name + "." + expr.field + "(...).");
    }
    if (auto en = exports.enums.find(expr.field); en != exports.enums.end()) {
      return makeEnum(en->second);
    }
    Binding* binding = moduleBinding(module, expr.field);
    if (!binding) {
      fail(expr.fieldSpan, "Module '" + exports.name + "' has no member '" + expr.field + "'.");
    }
    return binding->value;
  }

  auto* structValue = std::get_if<StructPtr>(&object.data);
  if (!structValue) fail(expr.span, "Only struct values have fields.");
  for (const auto& field : (*structValue)->fields) {
    if (field.first == expr.field) return field.second;
  }
  fail(expr.fieldSpan, "Struct '" + shortStructName((*structValue)->typeName) + "' has no field '" +
                           expr.field + "'.");
}

Interpreter::Binding* Interpreter::moduleBinding(const ModuleObj& module,
                                                 const std::string& member) {
  if (module.id < 0) return nullptr;
  return moduleEnv(module.id).find(member);
}

Value Interpreter::evalSafeNav(const SafeNavExpr& expr, Env& env) {
  Value object = eval(*expr.object, env);
  auto* optional = std::get_if<OptionalPtr>(&object.data);
  if (!optional) fail(expr.span, "'?.' requires an optional value.");
  if (!(*optional)->hasValue) return makeNone();

  Value inner = (*optional)->value;
  auto* structValue = std::get_if<StructPtr>(&inner.data);
  if (!structValue) fail(expr.span, "'?.' requires an optional struct value.");
  for (const auto& field : (*structValue)->fields) {
    if (field.first != expr.field) continue;
    if (std::holds_alternative<OptionalPtr>(field.second.data)) return field.second;
    return makeOptional(true, field.second);
  }
  fail(expr.fieldSpan, "Struct '" + shortStructName((*structValue)->typeName) +
                           "' has no field '" + expr.field + "'.");
}

Value Interpreter::evalTuple(const TupleExpr& expr, Env& env) {
  std::vector<Value> items;
  items.reserve(expr.elements.size());
  for (const auto& element : expr.elements) {
    items.push_back(eval(*element, env));
  }
  return makeTuple(std::move(items));
}

Value Interpreter::evalLambda(const LambdaExpr& expr, Env& env) {
  auto closure = std::make_shared<Closure>();
  closure->decl = &expr;
  closure->moduleId = currentModule_;
  // Capture enclosing locals, stopping at the module scope: module globals stay
  // live through the closure's parent environment.
  Env* moduleScope = &moduleEnv(currentModule_);
  for (Env* scope = &env; scope && scope != moduleScope; scope = scope->parent) {
    for (const auto& entry : scope->vars) {
      closure->captured.emplace(entry.first, entry.second.value);
    }
  }
  return makeClosureValue(std::move(closure));
}

Value Interpreter::evalMatch(const MatchExpr& expr, Env& env) {
  Value subject = eval(*expr.subject, env);

  for (const auto& arm : expr.arms) {
    bool matched = false;
    if (arm.pattern.kind == Pattern::Kind::Wildcard) {
      matched = true;
    } else if (arm.pattern.kind == Pattern::Kind::Literal) {
      matched = valuesEqual(subject, eval(*arm.pattern.value, env));
    } else {
      auto* value = std::get_if<std::int64_t>(&subject.data);
      if (value) {
        std::int64_t start =
            expectInt(eval(*arm.pattern.rangeStart, env), arm.pattern.rangeStart->span,
                      "The range pattern start");
        std::int64_t end = expectInt(eval(*arm.pattern.rangeEnd, env),
                                     arm.pattern.rangeEnd->span, "The range pattern end");
        matched = *value >= start && *value <= end;
      }
    }
    if (!matched) continue;
    if (arm.guard && !expectBool(eval(*arm.guard, env), arm.guard->span, "The match guard")) {
      continue;
    }
    return eval(*arm.result, env);
  }

  fail(expr.span, "No match arm matched value " + formatValue(subject, false) + ".");
}

Value Interpreter::evalTry(const TryExpr& expr, Env& env) {
  Value result = eval(*expr.operand, env);
  if (auto* res = std::get_if<ResultPtr>(&result.data)) {
    if (!(*res)->ok) {
      returnValue_ = (*res)->value;
      // Wrap in Err? The value stored should be the error payload; function expects Result.
      // Ensure returning an Err result.
      // If the function expects Result, returning the raw error would be wrong.
      // We stored the error payload; construct Err.
      returnValue_ = makeResult(false, (*res)->value);
      returning_ = true;
      return Value{};
    }
    return (*res)->value;
  }
  // If not a Result, just return the value.
  return result;
}

Value Interpreter::evalComprehension(const ComprehensionExpr& expr, Env& env) {
  Value iterable = eval(*expr.iterable, env);
  auto result = std::make_shared<std::vector<Value>>();

  // Reserve the exact capacity when the source length is known. Without this the
  // result vector reallocates and copies its Values several times per run.
  if (auto* range = std::get_if<RangeValue>(&iterable.data)) {
    if (range->step != 0 && range->end > range->start) {
      result->reserve(static_cast<std::size_t>((range->end - range->start + range->step - 1) /
                                               range->step));
    }
  } else if (auto* array = std::get_if<ArrayPtr>(&iterable.data)) {
    result->reserve((*array)->size());
  } else if (auto* text = std::get_if<std::string>(&iterable.data)) {
    result->reserve(text->size());
  }

  Env child;
  child.parent = &env;
  child.vars[expr.var].mutableBinding = false;

  auto runIteration = [&](Value element) {
    child.vars[expr.var].value = std::move(element);
    if (expr.condition && !expectBool(eval(*expr.condition, child), expr.condition->span,
                                      "The comprehension condition")) {
      return;
    }
    result->push_back(eval(*expr.element, child));
  };

  if (auto* range = std::get_if<RangeValue>(&iterable.data)) {
    for (std::int64_t i = range->start; i < range->end; i += range->step) {
      runIteration(makeInt(i));
      if (i > std::numeric_limits<std::int64_t>::max() - range->step) break;
    }
    return makeArray(std::move(result));
  }
  if (auto* array = std::get_if<ArrayPtr>(&iterable.data)) {
    const auto snapshot = **array;
    for (const auto& element : snapshot) runIteration(element);
    return makeArray(std::move(result));
  }
  if (auto* text = std::get_if<std::string>(&iterable.data)) {
    std::size_t index = 0;
    char32_t codepoint = 0;
    while (decodeUtf8(*text, index, codepoint)) runIteration(makeChar(codepoint));
    return makeArray(std::move(result));
  }
  fail(expr.iterable->span, "This value cannot be iterated over.");
}

Value Interpreter::evalBinary(const BinaryExpr& expr, Env& env) {
  switch (expr.binop) {
    case BinOp::Coalesce: {
      Value left = eval(*expr.lhs, env);
      auto* optional = std::get_if<OptionalPtr>(&left.data);
      if (!optional) fail(expr.opSpan, "The left side of '\?\?' must be optional.");
      if ((*optional)->hasValue) return (*optional)->value;
      return eval(*expr.rhs, env);
    }
    case BinOp::And: {
      bool left = expectBool(eval(*expr.lhs, env), expr.lhs->span, "The left operand of 'and'");
      if (!left) return makeBool(false);
      return makeBool(expectBool(eval(*expr.rhs, env), expr.rhs->span, "The right operand of 'and'"));
    }
    case BinOp::Or: {
      bool left = expectBool(eval(*expr.lhs, env), expr.lhs->span, "The left operand of 'or'");
      if (left) return makeBool(true);
      return makeBool(expectBool(eval(*expr.rhs, env), expr.rhs->span, "The right operand of 'or'"));
    }
    case BinOp::Range: {
      std::int64_t start = expectInt(eval(*expr.lhs, env), expr.lhs->span, "The range start");
      std::int64_t end = expectInt(eval(*expr.rhs, env), expr.rhs->span, "The range end");
      return makeRange(start, end);
    }
    default:
      break;
  }
  Value left = eval(*expr.lhs, env);
  Value right = eval(*expr.rhs, env);
  return applyBinary(expr.binop, left, right, expr.opSpan);
}

bool Interpreter::valuesEqual(const Value& lhs, const Value& rhs) {
  if (lhs.data.index() != rhs.data.index()) return false;
  if (std::holds_alternative<std::monostate>(lhs.data)) return true;
  if (auto* left = std::get_if<bool>(&lhs.data)) return *left == std::get<bool>(rhs.data);
  if (auto* left = std::get_if<std::int64_t>(&lhs.data)) {
    return *left == std::get<std::int64_t>(rhs.data);
  }
  if (auto* left = std::get_if<double>(&lhs.data)) return *left == std::get<double>(rhs.data);
  if (auto* left = std::get_if<char32_t>(&lhs.data)) {
    return *left == std::get<char32_t>(rhs.data);
  }
  if (auto* left = std::get_if<std::string>(&lhs.data)) {
    return *left == std::get<std::string>(rhs.data);
  }
  if (auto* left = std::get_if<OptionalPtr>(&lhs.data)) {
    const OptionalPtr& right = std::get<OptionalPtr>(rhs.data);
    if ((*left)->hasValue != right->hasValue) return false;
    if (!(*left)->hasValue) return true;
    return valuesEqual((*left)->value, right->value);
  }
  if (auto* left = std::get_if<ResultPtr>(&lhs.data)) {
    const ResultPtr& right = std::get<ResultPtr>(rhs.data);
    if ((*left)->ok != right->ok) return false;
    return valuesEqual((*left)->value, right->value);
  }
  if (auto* left = std::get_if<TuplePtr>(&lhs.data)) {
    const TuplePtr& right = std::get<TuplePtr>(rhs.data);
    if ((*left)->items.size() != right->items.size()) return false;
    for (std::size_t i = 0; i < (*left)->items.size(); ++i) {
      if (!valuesEqual((*left)->items[i], right->items[i])) return false;
    }
    return true;
  }
  if (auto* left = std::get_if<RangeValue>(&lhs.data)) {
    const RangeValue& right = std::get<RangeValue>(rhs.data);
    return left->start == right.start && left->end == right.end && left->step == right.step;
  }
  if (auto* left = std::get_if<ArrayPtr>(&lhs.data)) {
    return *left == std::get<ArrayPtr>(rhs.data);
  }
  if (auto* left = std::get_if<StructPtr>(&lhs.data)) {
    return *left == std::get<StructPtr>(rhs.data);
  }
  if (auto* left = std::get_if<ClosurePtr>(&lhs.data)) {
    return *left == std::get<ClosurePtr>(rhs.data);
  }
  return false;
}

Value Interpreter::applyBinary(BinOp op, const Value& lhs, const Value& rhs, Span span) {
  if (op == BinOp::In || op == BinOp::NotIn) {
    bool found = false;
    if (auto* array = std::get_if<ArrayPtr>(&rhs.data)) {
      for (const auto& item : **array) {
        if (valuesEqual(lhs, item)) {
          found = true;
          break;
        }
      }
    } else if (auto* range = std::get_if<RangeValue>(&rhs.data)) {
      if (auto* val = std::get_if<std::int64_t>(&lhs.data)) {
        if (range->step > 0 && *val >= range->start && *val < range->end && ((*val - range->start) % range->step == 0)) {
          found = true;
        }
      }
    } else if (auto* str = std::get_if<std::string>(&rhs.data)) {
      if (auto* sub = std::get_if<std::string>(&lhs.data)) {
        found = (str->find(*sub) != std::string::npos);
      }
    } else {
      fail(span, "Right operand of 'in' must be an array, range, or string.");
    }
    return makeBool(op == BinOp::In ? found : !found);
  }

  if (auto* left = std::get_if<OptionalPtr>(&lhs.data)) {
    auto* right = std::get_if<OptionalPtr>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between optional and non-optional.");
    if (op == BinOp::Eq || op == BinOp::Ne) {
      bool equal = false;
      if ((*left)->hasValue == (*right)->hasValue) {
        equal = !(*left)->hasValue || valuesEqual((*left)->value, (*right)->value);
      }
      return makeBool(op == BinOp::Eq ? equal : !equal);
    }
    fail(span, "Operator '" + std::string(binOpName(op)) + "' is not defined for optionals.");
  }

  if (auto* left = std::get_if<std::int64_t>(&lhs.data)) {
    auto* right = std::get_if<std::int64_t>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between integer and non-integer.");
    std::int64_t result = 0;
    if (op == BinOp::Add) {
      if (__builtin_add_overflow(*left, *right, &result)) fail(span, "Integer overflow in '+'.");
      return makeInt(result);
    }
    if (op == BinOp::Sub) {
      if (__builtin_sub_overflow(*left, *right, &result)) fail(span, "Integer overflow in '-'.");
      return makeInt(result);
    }
    if (op == BinOp::Mul) {
      if (__builtin_mul_overflow(*left, *right, &result)) fail(span, "Integer overflow in '*'.");
      return makeInt(result);
    }
    if (op == BinOp::Div) {
      if (*right == 0) fail(span, "Division by zero.", "Check the divisor before dividing.");
      if (*left == std::numeric_limits<std::int64_t>::min() && *right == -1) {
        fail(span, "Integer overflow in '/'.");
      }
      return makeInt(*left / *right);
    }
    if (op == BinOp::Mod) {
      if (*right == 0) fail(span, "Remainder by zero.", "Check the divisor before dividing.");
      if (*right == -1) return makeInt(0);
      return makeInt(*left % *right);
    }
    if (op == BinOp::Pow) {
      if (*right < 0) {
        fail(span, "Integer exponent must not be negative.",
             "Use floats (e.g. 2.0 ** -1) for negative exponents.");
      }
      std::int64_t base = *left;
      std::int64_t exponent = *right;
      std::int64_t power = 1;
      while (exponent > 0) {
        if (exponent & 1) {
          if (__builtin_mul_overflow(power, base, &power)) fail(span, "Integer overflow in '**'.");
        }
        exponent >>= 1;
        if (exponent > 0 && __builtin_mul_overflow(base, base, &base)) {
          fail(span, "Integer overflow in '**'.");
        }
      }
      return makeInt(power);
    }
    if (op == BinOp::BitAnd) return makeInt(*left & *right);
    if (op == BinOp::BitOr) return makeInt(*left | *right);
    if (op == BinOp::BitXor) return makeInt(*left ^ *right);
    if (op == BinOp::Shl) {
      if (*right < 0 || *right > 63) {
        fail(span, "Shift amount " + std::to_string(*right) + " is out of range for integers.",
             "Shift amounts must be between 0 and 63.");
      }
      return makeInt(static_cast<std::int64_t>(static_cast<std::uint64_t>(*left) << *right));
    }
    if (op == BinOp::Shr) {
      if (*right < 0 || *right > 63) {
        fail(span, "Shift amount " + std::to_string(*right) + " is out of range for integers.",
             "Shift amounts must be between 0 and 63.");
      }
      return makeInt(*left >> *right);
    }
    if (op == BinOp::Eq) return makeBool(*left == *right);
    if (op == BinOp::Ne) return makeBool(*left != *right);
    if (op == BinOp::Lt) return makeBool(*left < *right);
    if (op == BinOp::Gt) return makeBool(*left > *right);
    if (op == BinOp::Le) return makeBool(*left <= *right);
    if (op == BinOp::Ge) return makeBool(*left >= *right);
    fail(span, "Operator '" + std::string(binOpName(op)) + "' is not defined for integers.");
  }

  if (auto* left = std::get_if<double>(&lhs.data)) {
    auto* right = std::get_if<double>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between float and non-float.");
    if (op == BinOp::Add) return makeFloat(*left + *right);
    if (op == BinOp::Sub) return makeFloat(*left - *right);
    if (op == BinOp::Mul) return makeFloat(*left * *right);
    if (op == BinOp::Div) return makeFloat(*left / *right);
    if (op == BinOp::Pow) return makeFloat(std::pow(*left, *right));
    if (op == BinOp::Eq) return makeBool(*left == *right);
    if (op == BinOp::Ne) return makeBool(*left != *right);
    if (op == BinOp::Lt) return makeBool(*left < *right);
    if (op == BinOp::Gt) return makeBool(*left > *right);
    if (op == BinOp::Le) return makeBool(*left <= *right);
    if (op == BinOp::Ge) return makeBool(*left >= *right);
    fail(span, "Operator '" + std::string(binOpName(op)) + "' is not defined for floats.");
  }

  if (auto* left = std::get_if<std::string>(&lhs.data)) {
    auto* right = std::get_if<std::string>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between string and non-string.");
    if (op == BinOp::Add) return makeString(*left + *right);
    if (op == BinOp::Eq) return makeBool(*left == *right);
    if (op == BinOp::Ne) return makeBool(*left != *right);
    if (op == BinOp::Lt) return makeBool(*left < *right);
    if (op == BinOp::Gt) return makeBool(*left > *right);
    if (op == BinOp::Le) return makeBool(*left <= *right);
    if (op == BinOp::Ge) return makeBool(*left >= *right);
    fail(span, "Operator '" + std::string(binOpName(op)) + "' is not defined for strings.");
  }

  if (auto* left = std::get_if<char32_t>(&lhs.data)) {
    auto* right = std::get_if<char32_t>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between characters.");
    if (op == BinOp::Eq) return makeBool(*left == *right);
    if (op == BinOp::Ne) return makeBool(*left != *right);
    if (op == BinOp::Lt) return makeBool(*left < *right);
    if (op == BinOp::Gt) return makeBool(*left > *right);
    if (op == BinOp::Le) return makeBool(*left <= *right);
    if (op == BinOp::Ge) return makeBool(*left >= *right);
    fail(span, "Operator '" + std::string(binOpName(op)) + "' is not defined for characters.");
  }

  if (auto* left = std::get_if<bool>(&lhs.data)) {
    auto* right = std::get_if<bool>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between booleans.");
    if (op == BinOp::Eq) return makeBool(*left == *right);
    if (op == BinOp::Ne) return makeBool(*left != *right);
    fail(span, "Operator '" + std::string(binOpName(op)) + "' is not defined for bools.");
  }

  fail(span, "Operator '" + std::string(binOpName(op)) + "' cannot be applied to these values.");
}

// ---------------------------------------------------------------- calls

Value Interpreter::evalCall(const CallExpr& expr, Env& env) {
  if (auto* ident = asExpr<IdentExpr>(*expr.callee, ExprKind::Ident)) {
    // Sema proved the callee is a builtin, not a variable: evaluate arguments
    // and dispatch on the resolved id instead of walking scopes and name maps.
    if (expr.builtinId != BuiltinId::None) {
      ArgList args;
      args.reserve(expr.args.size());
      for (const auto& arg : expr.args) args.push_back(eval(*arg.value, env));
      return callBuiltinFast(expr.builtinId, ident->name, args, expr.span);
    }
    // Sema already resolved a plain identifier to a declared function or struct;
    // call it without walking the scope chain again.
    if (expr.calleeResolved && (expr.calleeFunction || expr.calleeStruct)) {
      if (expr.calleeStruct) {
        return constructStruct(*expr.calleeStruct, expr.args, env, expr.span);
      }
      ArgList args;
      args.reserve(expr.args.size());
      for (const auto& arg : expr.args) args.push_back(eval(*arg.value, env));
      return callFunction(*expr.calleeFunction, std::move(args), expr.span);
    }

    if (Binding* binding = env.find(ident->name)) {
      ArgList args;
      args.reserve(expr.args.size());
      for (const auto& arg : expr.args) args.push_back(eval(*arg.value, env));
      return callValue(binding->value, std::move(args), expr.span);
    }

    auto structIt = sema_.structs.end();
    if (!expr.calleeResolved) {
      structIt = sema_.structs.find(qualifiedName(ident->name));
      if (structIt == sema_.structs.end() && currentModule_ >= 0) {
        const ModuleExports& current = sema_.modules[static_cast<std::size_t>(currentModule_)];
        auto alias = current.structAliases.find(ident->name);
        if (alias != current.structAliases.end()) structIt = sema_.structs.find(alias->second);
      }
      expr.calleeStruct = structIt != sema_.structs.end() ? structIt->second : nullptr;
      auto fnIt = sema_.functions.find(qualifiedName(ident->name));
      expr.calleeFunction = fnIt != sema_.functions.end() ? fnIt->second : nullptr;
      expr.calleeResolved = true;
    }
    if (expr.calleeStruct) {
      return constructStruct(*expr.calleeStruct, expr.args, env, expr.span);
    }

    ArgList args;
    args.reserve(expr.args.size());
    for (const auto& arg : expr.args) args.push_back(eval(*arg.value, env));

    if (expr.calleeFunction) {
      return callFunction(*expr.calleeFunction, std::move(args), expr.span);
    }
    return callBuiltin(ident->name, args, expr.span);
  }

  if (auto* field = asExpr<FieldExpr>(*expr.callee, ExprKind::Field)) {
    Value object = eval(*field->object, env);

    if (auto* modulePtr = std::get_if<ModulePtr>(&object.data)) {
      return callModuleMember(**modulePtr, field->field, expr.args, env, expr.span);
    }

    ArgList args;
    args.reserve(expr.args.size());
    for (const auto& arg : expr.args) args.push_back(eval(*arg.value, env));

    if (auto* structValue = std::get_if<StructPtr>(&object.data)) {
      return callMethod(*structValue, field->field, std::move(args), expr.span);
    }
    return callBuiltinMethod(object, field->field, args, expr.span);
  }

  Value callee = eval(*expr.callee, env);
  ArgList args;
  args.reserve(expr.args.size());
  for (const auto& arg : expr.args) args.push_back(eval(*arg.value, env));
  return callValue(callee, std::move(args), expr.span);
}

Value Interpreter::callModuleMember(const ModuleObj& module, const std::string& member,
                                    const std::vector<CallArg>& args, Env& env, Span span) {
  if (module.id < 0) {
    ArgList values;
    values.reserve(args.size());
    for (const auto& arg : args) values.push_back(eval(*arg.value, env));
    if (const std::string* fn = nativeFunction(module.nativeName, member)) {
      return callBuiltin(*fn, values, span);
    }
    if (nativeConstant(module.nativeName, member)) {
      fail(span, "'" + module.nativeName + "." + member + "' is a constant, not a function.");
    }
    fail(span, "Module '" + module.nativeName + "' has no function '" + member + "'.");
  }

  const ModuleExports& exports = sema_.modules[static_cast<std::size_t>(module.id)];
  if (auto fn = exports.functions.find(member); fn != exports.functions.end()) {
    ArgList values;
    values.reserve(args.size());
    for (const auto& arg : args) values.push_back(eval(*arg.value, env));
    return callFunction(*fn->second, std::move(values), span);
  }
  if (exports.structs.count(member)) {
    const StructDecl* decl = findStruct(qualifiedName(module.id, member));
    if (!decl) fail(span, "Internal error: unknown struct '" + member + "'.");
    return constructStruct(*decl, args, env, span);
  }

  Binding* binding = moduleBinding(module, member);
  if (binding) {
    ArgList values;
    values.reserve(args.size());
    for (const auto& arg : args) values.push_back(eval(*arg.value, env));
    return callValue(binding->value, std::move(values), span);
  }

  fail(span, "Module '" + exports.name + "' has no function '" + member + "'.");
}

Value Interpreter::callValue(const Value& callee, ArgList args, Span span) {
  if (auto* closure = std::get_if<ClosurePtr>(&callee.data)) {
    return callClosure(*closure, std::move(args), span);
  }
  if (auto* native = std::get_if<NativeFunctionPtr>(&callee.data)) {
    return callBuiltin((*native)->builtinName, args, span);
  }
  fail(span, "This value is not callable.",
       "Only functions and lambdas can be called.");
}

Value Interpreter::callClosure(const ClosurePtr& closure, ArgList args, Span span) {
  if (closure->fn) return callFunction(*closure->fn, std::move(args), span);
  const LambdaExpr& decl = *closure->decl;
  if (args.size() != decl.params.size()) {
    fail(span,
         "This function expects " + std::to_string(decl.params.size()) + " argument(s), found " +
             std::to_string(args.size()) + ".");
  }

  CallDepthGuard guard(callDepth_);
  if (callDepth_ > kMaxCallDepth) {
    fail(span, "Maximum call depth of " + std::to_string(kMaxCallDepth) + " exceeded.",
         "Check for infinite recursion.");
  }

  int savedModule = currentModule_;
  if (closure->moduleId >= 0) currentModule_ = closure->moduleId;

  Env frame;
  frame.parent = &moduleEnv(currentModule_);
  for (const auto& entry : closure->captured) {
    Binding binding;
    binding.value = entry.second;
    binding.mutableBinding = false;
    frame.vars[entry.first] = std::move(binding);
  }
  for (std::size_t i = 0; i < decl.params.size(); ++i) {
    Binding binding;
    binding.value = std::move(args[i]);
    binding.mutableBinding = false;
    frame.vars[decl.params[i].name] = std::move(binding);
  }
  Value result = eval(*decl.body, frame);
  currentModule_ = savedModule;
  return result;
}

Value Interpreter::callFunction(const FnDecl& decl, ArgList args, Span span) {
  if (args.size() != decl.params.size()) {
    fail(span, "Function '" + decl.name + "' expects " + std::to_string(decl.params.size()) +
                   " argument(s), found " + std::to_string(args.size()) + ".");
  }

  CallDepthGuard guard(callDepth_);
  if (callDepth_ > kMaxCallDepth) {
    fail(span, "Maximum call depth of " + std::to_string(kMaxCallDepth) + " exceeded.",
         "Check for infinite recursion.");
  }

  int savedModule = currentModule_;
  if (decl.moduleId >= 0) currentModule_ = decl.moduleId;

  Env frame;
  frame.parent = &moduleEnv(currentModule_);
  for (std::size_t i = 0; i < decl.params.size(); ++i) {
    Binding binding;
    binding.value = std::move(args[i]);
    binding.mutableBinding = false;
    frame.vars[decl.params[i].name] = std::move(binding);
  }

  bool outerReturning = returning_;
  returning_ = false;
  execStatements(decl.body, frame);
  Value result = returning_ ? std::move(returnValue_) : makeVoid();
  returning_ = outerReturning;
  currentModule_ = savedModule;
  return result;
}

Value Interpreter::callMethod(const StructPtr& object, const std::string& methodName,
                              ArgList args, Span span) {
  const StructDecl* decl = findStruct(object->typeName);
  if (!decl) fail(span, "Internal error: unknown struct '" + object->typeName + "'.");

  for (const StructDecl* curr = decl; curr; curr = curr->baseClass.empty() ? nullptr : findStruct(curr->baseClass.find("::") != std::string::npos ? curr->baseClass : qualifiedName(curr->baseClass))) {
    for (const auto& method : curr->methods) {
      if (method->name != methodName) continue;
      ArgList all;
      all.reserve(args.size() + 1);
      all.push_back(makeStructValue(object));
      for (auto& arg : args) all.push_back(std::move(arg));
      return callFunction(*method, std::move(all), span);
    }
  }

  for (const auto& field : object->fields) {
    if (field.first != methodName) continue;
    if (auto* closure = std::get_if<ClosurePtr>(&field.second.data)) {
      return callClosure(*closure, std::move(args), span);
    }
  }

  fail(span, "Struct '" + shortStructName(object->typeName) + "' has no method '" + methodName +
                 "'.");
}

Value Interpreter::constructStruct(const StructDecl& decl, const std::vector<CallArg>& args,
                                   Env& env, Span span) {
  // Collect all field names and spans including base classes
  struct FieldInfo {
    std::string name;
    Span span;
  };
  std::vector<FieldInfo> allFields;
  if (!decl.baseClass.empty()) {
    std::string baseQ = decl.baseClass.find("::") != std::string::npos ? decl.baseClass : qualifiedName(decl.baseClass);
    const StructDecl* baseDecl = findStruct(baseQ);
    if (!baseDecl) baseDecl = findStruct(decl.baseClass);
    if (baseDecl) {
      for (const auto& bf : baseDecl->fields) allFields.push_back({bf.name, bf.span});
    }
  }
  for (const auto& f : decl.fields) allFields.push_back({f.name, f.span});

  std::vector<Value> values(allFields.size());
  std::vector<bool> provided(allFields.size(), false);

  std::size_t nextPositional = 0;
  for (const auto& arg : args) {
    if (arg.name.empty()) {
      if (nextPositional >= allFields.size()) {
        fail(span, "Too many positional arguments for struct '" + decl.name + "'.");
      }
      values[nextPositional] = eval(*arg.value, env);
      provided[nextPositional] = true;
      ++nextPositional;
      continue;
    }

    std::size_t index = allFields.size();
    for (std::size_t i = 0; i < allFields.size(); ++i) {
      if (allFields[i].name == arg.name) {
        index = i;
        break;
      }
    }
    if (index == allFields.size()) {
      fail(arg.nameSpan, "Struct '" + decl.name + "' has no field '" + arg.name + "'.");
    }
    values[index] = eval(*arg.value, env);
    provided[index] = true;
  }

  auto object = std::make_shared<StructObj>();
  auto moduleIt = sema_.structModule.find(&decl);
  object->typeName = moduleIt != sema_.structModule.end()
                         ? qualifiedName(moduleIt->second, decl.name)
                         : decl.name;
  object->fields.reserve(allFields.size());
  for (std::size_t i = 0; i < allFields.size(); ++i) {
    if (!provided[i]) {
      fail(span, "Missing field '" + allFields[i].name + "' in struct '" + decl.name + "'.");
    }
    object->fields.emplace_back(allFields[i].name, std::move(values[i]));
  }
  return makeStructValue(std::move(object));
}

const StructDecl* Interpreter::findStruct(const std::string& name) const {
  auto found = sema_.structs.find(name);
  return found == sema_.structs.end() ? nullptr : found->second;
}

namespace {

// Join an array into one string. The common case is an array of strings, where
// the total size is known: reserve once and append the values directly instead
// of formatting (and copying) each element into a temporary.
std::string joinValues(const std::vector<Value>& values, const std::string& separator) {
  std::size_t total = values.empty() ? 0 : separator.size() * (values.size() - 1);
  bool allStrings = true;
  for (const Value& value : values) {
    if (auto* text = std::get_if<std::string>(&value.data)) {
      total += text->size();
    } else {
      allStrings = false;
      break;
    }
  }

  std::string out;
  if (allStrings) {
    out.reserve(total);
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i > 0) out += separator;
      out += *std::get_if<std::string>(&values[i].data);
    }
    return out;
  }

  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += separator;
    out += formatValue(values[i], false);
  }
  return out;
}

}  // namespace

Value Interpreter::callBuiltinFast(BuiltinId id, const std::string& name, ArgList& args,
                                   Span span) {
  // Monomorphic fast paths for the builtins that appear in hot loops. Anything
  // that is not exactly the common shape (wrong arity or type) falls through to
  // the general implementation so diagnostics stay identical.
  if (args.size() == 1) {
    Value& value = args[0];
    switch (id) {
      case BuiltinId::F64:
        if (auto* number = std::get_if<double>(&value.data)) return makeFloat(*number);
        if (auto* number = std::get_if<std::int64_t>(&value.data)) {
          return makeFloat(static_cast<double>(*number));
        }
        break;
      case BuiltinId::I64:
        if (auto* number = std::get_if<std::int64_t>(&value.data)) return makeInt(*number);
        break;
      case BuiltinId::Sqrt:
        if (auto* number = std::get_if<double>(&value.data)) return makeFloat(std::sqrt(*number));
        if (auto* number = std::get_if<std::int64_t>(&value.data)) {
          return makeFloat(std::sqrt(static_cast<double>(*number)));
        }
        break;
      case BuiltinId::Round:
        if (auto* number = std::get_if<double>(&value.data)) return makeFloat(std::round(*number));
        break;
      case BuiltinId::Floor:
        if (auto* number = std::get_if<double>(&value.data)) return makeFloat(std::floor(*number));
        break;
      case BuiltinId::Ceil:
        if (auto* number = std::get_if<double>(&value.data)) return makeFloat(std::ceil(*number));
        break;
      case BuiltinId::Str:
        return makeString(formatValue(value, false));
      case BuiltinId::Len:
        if (auto* text = std::get_if<std::string>(&value.data)) {
          return makeInt(static_cast<std::int64_t>(codepointCount(*text)));
        }
        if (auto* array = std::get_if<ArrayPtr>(&value.data)) {
          return makeInt(static_cast<std::int64_t>((*array)->size()));
        }
        break;
      case BuiltinId::Abs:
        if (auto* number = std::get_if<std::int64_t>(&value.data)) {
          if (*number != std::numeric_limits<std::int64_t>::min()) {
            return makeInt(*number < 0 ? -*number : *number);
          }
        }
        break;
      default:
        break;
    }
  }
  return callBuiltin(name, args, span);
}

Value Interpreter::callBuiltin(const std::string& name, ArgList& args, Span span) {
  // Hottest builtins first: numeric conversion, math, and string/collection
  // helpers otherwise sit behind dozens of name comparisons in this chain.
  if (name == "f64") {
    if (args.size() != 1) fail(span, "'f64' expects 1 argument.");
    if (auto* value = std::get_if<double>(&args[0].data)) return makeFloat(*value);
    if (auto* value = std::get_if<std::int64_t>(&args[0].data)) {
      return makeFloat(static_cast<double>(*value));
    }
    fail(span, "'f64' requires a numeric argument.");
  }
  if (name == "i64") {
    if (args.size() != 1) fail(span, "'i64' expects 1 argument.");
    return convertNumber("i64", args[0], span);
  }
  if (name == "sqrt") {
    if (args.size() != 1) fail(span, "'sqrt' expects 1 argument.");
    return makeFloat(std::sqrt(expectFloat(args[0], span, "sqrt")));
  }
  if (name == "str") {
    if (args.size() != 1) fail(span, "'str' expects 1 argument.");
    return makeString(formatValue(args[0], false));
  }
  if (name == "len") {
    if (args.size() != 1) fail(span, "'len' expects 1 argument.");
    if (auto* text = std::get_if<std::string>(&args[0].data)) {
      return makeInt(static_cast<std::int64_t>(codepointCount(*text)));
    }
    if (auto* array = std::get_if<ArrayPtr>(&args[0].data)) {
      return makeInt(static_cast<std::int64_t>((*array)->size()));
    }
    fail(span, "'len' requires a string or an array.");
  }
  if (name == "print" || name == "eprint") {
    std::string line;
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i) line += " ";
      line += formatValue(args[i], false);
    }
    if (name == "print") {
      out_ << line << '\n';
    } else {
      err_ << line << '\n';
      err_.flush();
    }
    return makeVoid();
  }

  if (name == "input") {
    if (!args.empty()) {
      out_ << expectString(args[0], span, "The input prompt");
      out_.flush();
    }
    std::string line;
    if (!std::getline(in_, line)) {
      fail(span, "Input ended before a line could be read.",
           "Provide input on standard input before running the program.");
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return makeString(std::move(line));
  }

  if (name == "len") {
    if (args.size() != 1) fail(span, "'len' expects 1 argument.");
    if (auto* text = std::get_if<std::string>(&args[0].data)) {
      return makeInt(static_cast<std::int64_t>(codepointCount(*text)));
    }
    if (auto* array = std::get_if<ArrayPtr>(&args[0].data)) {
      return makeInt(static_cast<std::int64_t>((*array)->size()));
    }
    fail(span, "'len' requires a string or an array.");
  }

  if (name == "str") {
    if (args.size() != 1) fail(span, "'str' expects 1 argument.");
    return makeString(formatValue(args[0], false));
  }

  if (name == "abs") {
    if (args.size() != 1) fail(span, "'abs' expects 1 argument.");
    if (auto* value = std::get_if<std::int64_t>(&args[0].data)) {
      if (*value == std::numeric_limits<std::int64_t>::min()) {
        fail(span, "Integer overflow in 'abs'.");
      }
      return makeInt(*value < 0 ? -*value : *value);
    }
    if (auto* value = std::get_if<double>(&args[0].data)) return makeFloat(std::fabs(*value));
    fail(span, "'abs' requires a numeric argument.");
  }

  if (name == "min" || name == "max") {
    if (args.size() != 2) fail(span, "'" + name + "' expects 2 arguments.");
    if (auto* left = std::get_if<std::int64_t>(&args[0].data)) {
      auto* right = std::get_if<std::int64_t>(&args[1].data);
      if (!right) fail(span, "'" + name + "' requires matching numeric arguments.");
      if (name == "min") return makeInt(std::min(*left, *right));
      return makeInt(std::max(*left, *right));
    }
    if (auto* left = std::get_if<double>(&args[0].data)) {
      auto* right = std::get_if<double>(&args[1].data);
      if (!right) fail(span, "'" + name + "' requires matching numeric arguments.");
      if (name == "min") return makeFloat(std::min(*left, *right));
      return makeFloat(std::max(*left, *right));
    }
    fail(span, "'" + name + "' requires numeric arguments.");
  }

  if (name == "pow") {
    if (args.size() != 2) fail(span, "'pow' expects 2 arguments.");
    return makeFloat(std::pow(expectFloat(args[0], span, "The base of 'pow'"),
                              expectFloat(args[1], span, "The exponent of 'pow'")));
  }

  if (name == "sqrt") return makeFloat(std::sqrt(expectFloat(args.at(0), span, "sqrt")));
  if (name == "sin") return makeFloat(std::sin(expectFloat(args.at(0), span, "sin")));
  if (name == "cos") return makeFloat(std::cos(expectFloat(args.at(0), span, "cos")));
  if (name == "tan") return makeFloat(std::tan(expectFloat(args.at(0), span, "tan")));
  if (name == "exp") return makeFloat(std::exp(expectFloat(args.at(0), span, "exp")));
  if (name == "log") return makeFloat(std::log(expectFloat(args.at(0), span, "log")));
  if (name == "floor") return makeFloat(std::floor(expectFloat(args.at(0), span, "floor")));
  if (name == "ceil") return makeFloat(std::ceil(expectFloat(args.at(0), span, "ceil")));
  if (name == "round") return makeFloat(std::round(expectFloat(args.at(0), span, "round")));
  if (name == "trunc") return makeFloat(std::trunc(expectFloat(args.at(0), span, "trunc")));
  if (name == "log2") return makeFloat(std::log2(expectFloat(args.at(0), span, "log2")));
  if (name == "log10") return makeFloat(std::log10(expectFloat(args.at(0), span, "log10")));
  if (name == "atan2") {
    if (args.size() != 2) fail(span, "'atan2' expects 2 arguments.");
    return makeFloat(std::atan2(expectFloat(args[0], span, "The y argument of 'atan2'"),
                                expectFloat(args[1], span, "The x argument of 'atan2'")));
  }
  if (name == "random") {
    if (!args.empty()) fail(span, "'random' takes no arguments.");
    std::uniform_real_distribution<double> distribution(0.0, 1.0);
    return makeFloat(distribution(randomEngine()));
  }
  if (name == "random_int") {
    if (args.size() != 2) fail(span, "'random_int' expects 2 arguments.");
    std::int64_t low = expectInt(args[0], span, "The low bound of 'random_int'");
    std::int64_t high = expectInt(args[1], span, "The high bound of 'random_int'");
    if (low > high) {
      fail(span, "'random_int' requires the low bound to be at most the high bound.",
           "Write random_int(0, 9) for numbers from 0 through 9.");
    }
    std::uniform_int_distribution<std::int64_t> distribution(low, high);
    return makeInt(distribution(randomEngine()));
  }

  if (name == "flush") {
    if (!args.empty()) fail(span, "'flush' takes no arguments.");
    out_.flush();
    err_.flush();
    return makeVoid();
  }

  if (name == "read_file") {
    if (args.size() != 1) fail(span, "'read_file' expects 1 argument.");
    const std::string& path = expectString(args[0], span, "The 'read_file' path");
    FileResult result = readFile(path);
    if (!result.ok) {
      fail(span, result.error, "Check that the file exists and is readable.");
    }
    return makeString(std::move(result.value));
  }

  if (name == "write_file" || name == "append_file") {
    if (args.size() != 2) fail(span, "'" + name + "' expects 2 arguments.");
    const std::string& path = expectString(args[0], span, "The file path");
    const std::string& contents = expectString(args[1], span, "The file contents");
    FileResult result = writeFile(path, contents, name == "append_file");
    if (!result.ok) {
      fail(span, result.error, "Check that the directory exists and is writable.");
    }
    return makeVoid();
  }

  if (name == "file_exists") {
    if (args.size() != 1) fail(span, "'file_exists' expects 1 argument.");
    const std::string& path = expectString(args[0], span, "The 'file_exists' path");
    return makeBool(fileExists(path));
  }

  if (name == "list_dir") {
    if (args.size() != 1) fail(span, "'list_dir' expects 1 argument.");
    const std::string& path = expectString(args[0], span, "The 'list_dir' path");
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec);
    if (ec) {
      fail(span, "Cannot list directory \"" + path + "\": " + ec.message(),
           "Check that the directory exists and is readable.");
    }
    auto entries = std::make_shared<std::vector<Value>>();
    for (const auto& entry : it) entries->push_back(makeString(entry.path().filename().string()));
    std::sort(entries->begin(), entries->end(),
              [](const Value& a, const Value& b) {
                return std::get<std::string>(a.data) < std::get<std::string>(b.data);
              });
    return makeArray(std::move(entries));
  }

  if (name == "remove_file" || name == "create_dir" || name == "remove_dir") {
    if (args.size() != 1) fail(span, "'" + name + "' expects 1 argument.");
    const std::string& path = expectString(args[0], span, "The '" + name + "' path");
    std::error_code ec;
    if (name == "create_dir") {
      std::filesystem::create_directories(path, ec);
      if (!ec) return makeVoid();
      fail(span, "Cannot create directory \"" + path + "\": " + ec.message());
    }
    bool removed = std::filesystem::remove(path, ec);
    if (ec) fail(span, "Cannot remove \"" + path + "\": " + ec.message());
    if (!removed) fail(span, "\"" + path + "\" does not exist.");
    return makeVoid();
  }

  if (name == "rename") {
    if (args.size() != 2) fail(span, "'rename' expects 2 arguments.");
    const std::string& from = expectString(args[0], span, "The source path");
    const std::string& to = expectString(args[1], span, "The destination path");
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) fail(span, "Cannot rename \"" + from + "\" to \"" + to + "\": " + ec.message());
    return makeVoid();
  }

  if (name == "current_dir") {
    if (!args.empty()) fail(span, "'current_dir' takes no arguments.");
    std::error_code ec;
    std::filesystem::path path = std::filesystem::current_path(ec);
    if (ec) fail(span, "Cannot read the current directory: " + ec.message());
    return makeString(path.string());
  }

  if (name == "now") {
    if (!args.empty()) fail(span, "'now' takes no arguments.");
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return makeInt(static_cast<std::int64_t>(millis));
  }

  if (name == "sleep") {
    if (args.size() != 1) fail(span, "'sleep' expects 1 argument.");
    std::int64_t millis = expectInt(args[0], span, "The duration passed to 'sleep'");
    if (millis < 0) fail(span, "'sleep' requires a non-negative duration.");
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    return makeVoid();
  }

  if (name == "panic") {
    if (args.size() != 1) fail(span, "'panic' expects 1 argument.");
    fail(span, expectString(args[0], span, "The 'panic' message"));
  }

  if (name == "assert") {
    if (args.empty() || args.size() > 2) fail(span, "'assert' expects 1 or 2 arguments.");
    if (expectBool(args[0], span, "The 'assert' condition")) return makeVoid();
    std::string message = "Assertion failed";
    if (args.size() == 2) message += ": " + expectString(args[1], span, "The 'assert' message");
    fail(span, std::move(message));
  }

  if (name == "assert_eq") {
    if (args.size() < 2 || args.size() > 3) fail(span, "'assert_eq' expects 2 or 3 arguments.");
    if (valuesEqual(args[0], args[1])) return makeVoid();
    std::string message = "Assertion failed: expected " + formatValue(args[0], false) +
                          ", found " + formatValue(args[1], false);
    if (args.size() == 3) message += " (" + expectString(args[2], span, "The 'assert_eq' message") + ")";
    fail(span, std::move(message));
  }

  if (name == "Some") {
    if (args.size() != 1) fail(span, "'Some' expects 1 argument.");
    return makeOptional(true, std::move(args[0]));
  }

  if (name == "Ok") {
    if (args.size() != 1) fail(span, "'Ok' expects 1 argument.");
    return makeResult(true, std::move(args[0]));
  }

  if (name == "Err") {
    if (args.size() != 1) fail(span, "'Err' expects 1 argument.");
    return makeResult(false, std::move(args[0]));
  }

  if (name == "map" || name == "filter" || name == "any" || name == "all") {
    if (args.size() != 2) fail(span, "'" + name + "' expects 2 arguments.");
    ArrayPtr array = expectArray(args[0], span, "The first argument of '" + name + "'");
    const Value& function = args[1];
    if (name == "map") {
      auto result = std::make_shared<std::vector<Value>>();
      result->reserve(array->size());
      for (const auto& element : *array) {
        result->push_back(callValue(function, ArgList{element}, span));
      }
      return makeArray(std::move(result));
    }
    if (name == "filter") {
      auto result = std::make_shared<std::vector<Value>>();
      for (const auto& element : *array) {
        if (expectBool(callValue(function, ArgList{element}, span), span,
                       "The filter predicate")) {
          result->push_back(element);
        }
      }
      return makeArray(std::move(result));
    }
    for (const auto& element : *array) {
      bool value = expectBool(callValue(function, ArgList{element}, span),
                              span, "The '" + name + "' predicate");
      if (name == "any" && value) return makeBool(true);
      if (name == "all" && !value) return makeBool(false);
    }
    return makeBool(name == "all");
  }

  if (name == "reduce") {
    if (args.size() != 3) fail(span, "'reduce' expects 3 arguments.");
    ArrayPtr array = expectArray(args[0], span, "The first argument of 'reduce'");
    Value accumulator = args[1];
    const Value& function = args[2];
    for (const auto& element : *array) {
      accumulator = callValue(function, ArgList{accumulator, element}, span);
    }
    return accumulator;
  }

  if (name == "sum") {
    if (args.size() != 1) fail(span, "'sum' expects 1 argument.");
    ArrayPtr array = expectArray(args[0], span, "The argument of 'sum'");
    if (array->empty()) return makeInt(0);
    Value accumulator = array->front();
    if (auto* total = std::get_if<std::int64_t>(&accumulator.data)) {
      std::int64_t sum = *total;
      std::size_t i = 1;
      for (; i < array->size(); ++i) {
        auto* element = std::get_if<std::int64_t>(&(*array)[i].data);
        if (!element) break;
        std::int64_t next = 0;
        if (__builtin_add_overflow(sum, *element, &next)) break;
        sum = next;
      }
      if (i == array->size()) return makeInt(sum);
      accumulator = makeInt(sum);
    }
    for (std::size_t i = 1; i < array->size(); ++i) {
      accumulator = applyBinary(BinOp::Add, accumulator, (*array)[i], span);
    }
    return accumulator;
  }

  if (name == "join") {
    if (args.size() != 2) fail(span, "'join' expects 2 arguments (array, separator).");
    ArrayPtr array = expectArray(args[0], span, "The first argument of 'join'");
    const std::string& separator = expectString(args[1], span, "The 'join' separator");
    return makeString(joinValues(*array, separator));
  }

  if (name == "i8" || name == "i16" || name == "i32" || name == "i64" || name == "u8" ||
      name == "u16" || name == "u32" || name == "u64" || name == "f32" || name == "f64") {
    if (args.size() != 1) fail(span, "'" + name + "' expects 1 argument.");
    return convertNumber(name, args[0], span);
  }

  fail(span, "Internal error: unknown builtin function '" + name + "'.");
}

Value Interpreter::callBuiltinMethod(const Value& object, const std::string& method,
                                     ArgList& args, Span span) {
  if (auto* array = std::get_if<ArrayPtr>(&object.data)) {
    if (method == "push") {
      if (args.size() != 1) fail(span, "Method 'push' expects 1 argument.");
      (*array)->push_back(std::move(args[0]));
      return makeVoid();
    }
    if (method == "pop") {
      if (!(*array)->empty()) {
        Value value = std::move((*array)->back());
        (*array)->pop_back();
        return value;
      }
      fail(span, "Cannot pop from an empty array.");
    }
    if (method == "reverse") {
      if (!args.empty()) fail(span, "Method 'reverse' takes no arguments.");
      std::reverse((*array)->begin(), (*array)->end());
      return makeVoid();
    }
    if (method == "contains") {
      if (args.size() != 1) fail(span, "Method 'contains' expects 1 argument.");
      for (const auto& elem : **array) {
        if (valuesEqual(elem, args[0])) {
          return makeBool(true);
        }
      }
      return makeBool(false);
    }
    if (method == "join") {
      if (args.size() != 1) fail(span, "Method 'join' expects 1 argument (separator).");
      const std::string& sep = expectString(args[0], span, "The 'join' separator");
      return makeString(joinValues(**array, sep));
    }
    if (method == "insert") {
      if (args.size() != 2) fail(span, "Method 'insert' expects 2 arguments (index, value).");
      std::int64_t raw = expectInt(args[0], span, "The index passed to 'insert'");
      std::int64_t size = static_cast<std::int64_t>((*array)->size());
      if (raw < 0) raw += size;
      if (raw < 0 || raw > size) {
        fail(span, "Insertion index " + std::to_string(raw) +
                       " is out of bounds for an array of length " + std::to_string(size) + ".",
             "Use an index from 0 through the array length.");
      }
      (*array)->insert((*array)->begin() + raw, std::move(args[1]));
      return makeVoid();
    }
    if (method == "remove_at") {
      if (args.size() != 1) fail(span, "Method 'remove_at' expects 1 argument.");
      std::int64_t raw = expectInt(args[0], span, "The index passed to 'remove_at'");
      std::size_t index = static_cast<std::size_t>(
          normalizeIndex(raw, (*array)->size(), span));
      Value value = std::move((**array)[index]);
      (*array)->erase((*array)->begin() + static_cast<std::ptrdiff_t>(index));
      return value;
    }
    if (method == "index_of") {
      if (args.size() != 1) fail(span, "Method 'index_of' expects 1 argument.");
      for (std::size_t i = 0; i < (*array)->size(); ++i) {
        if (valuesEqual((**array)[i], args[0])) {
          return makeInt(static_cast<std::int64_t>(i));
        }
      }
      return makeInt(-1);
    }
    if (method == "sort") {
      if (!args.empty()) fail(span, "Method 'sort' takes no arguments.");
      std::sort((*array)->begin(), (*array)->end(), [](const Value& a, const Value& b) {
        if (auto* left = std::get_if<std::int64_t>(&a.data)) {
          return *left < std::get<std::int64_t>(b.data);
        }
        if (auto* left = std::get_if<double>(&a.data)) {
          return *left < std::get<double>(b.data);
        }
        return std::get<std::string>(a.data) < std::get<std::string>(b.data);
      });
      return makeVoid();
    }
    if (method == "first" || method == "last") {
      if (!args.empty()) fail(span, "Method '" + method + "' takes no arguments.");
      if ((*array)->empty()) return makeNone();
      return makeOptional(true, method == "first" ? (*array)->front() : (*array)->back());
    }
    fail(span, "Array has no method '" + method + "'.");
  }

  if (auto* text = std::get_if<std::string>(&object.data)) {
    if (method == "to_int") {
      if (!args.empty()) fail(span, "Method 'to_int' takes no arguments.");
      std::int64_t value = 0;
      if (!parseInteger(*text, value)) {
        fail(span, "Cannot parse \"" + *text + "\" as an integer.",
             "Check for whitespace or non-digit characters.");
      }
      return makeInt(value);
    }
    if (method == "to_float") {
      if (!args.empty()) fail(span, "Method 'to_float' takes no arguments.");
      double value = 0;
      if (!parseFloat(*text, value)) {
        fail(span, "Cannot parse \"" + *text + "\" as a float.");
      }
      return makeFloat(value);
    }
    if (method == "upper" || method == "lower") {
      if (!args.empty()) fail(span, "Method '" + method + "' takes no arguments.");
      std::string out = *text;
      for (char& c : out) {
        unsigned char byte = static_cast<unsigned char>(c);
        c = static_cast<char>(method == "upper" ? std::toupper(byte) : std::tolower(byte));
      }
      return makeString(std::move(out));
    }
    if (method == "trim") {
      if (!args.empty()) fail(span, "Method 'trim' takes no arguments.");
      std::size_t begin = 0;
      std::size_t end = text->size();
      while (begin < end && std::isspace(static_cast<unsigned char>((*text)[begin]))) ++begin;
      while (end > begin && std::isspace(static_cast<unsigned char>((*text)[end - 1]))) --end;
      return makeString(text->substr(begin, end - begin));
    }
    if (method == "contains") {
      if (args.size() != 1) fail(span, "Method 'contains' expects 1 argument.");
      const std::string& needle = expectString(args[0], span, "The 'contains' argument");
      return makeBool(text->find(needle) != std::string::npos);
    }
    if (method == "split") {
      if (args.size() != 1) fail(span, "Method 'split' expects 1 argument.");
      const std::string& separator = expectString(args[0], span, "The 'split' separator");
      if (separator.empty()) fail(span, "The 'split' separator must not be empty.");
      auto parts = std::make_shared<std::vector<Value>>();
      std::size_t start = 0;
      while (true) {
        std::size_t position = text->find(separator, start);
        if (position == std::string::npos) {
          parts->push_back(makeString(text->substr(start)));
          break;
        }
        parts->push_back(makeString(text->substr(start, position - start)));
        start = position + separator.size();
      }
      return makeArray(std::move(parts));
    }
    if (method == "replace") {
      if (args.size() != 2) fail(span, "Method 'replace' expects 2 arguments.");
      const std::string& needle = expectString(args[0], span, "The 'replace' target");
      const std::string& replacement = expectString(args[1], span, "The 'replace' replacement");
      if (needle.empty()) fail(span, "The 'replace' target must not be empty.");
      std::string out;
      std::size_t start = 0;
      while (true) {
        std::size_t position = text->find(needle, start);
        if (position == std::string::npos) {
          out += text->substr(start);
          break;
        }
        out += text->substr(start, position - start);
        out += replacement;
        start = position + needle.size();
      }
      return makeString(std::move(out));
    }
    if (method == "starts_with" || method == "ends_with") {
      if (args.size() != 1) fail(span, "Method '" + method + "' expects 1 argument.");
      const std::string& part = expectString(args[0], span, "The argument to '" + method + "'");
      if (method == "starts_with") {
        return makeBool(text->size() >= part.size() &&
                        text->compare(0, part.size(), part) == 0);
      }
      return makeBool(text->size() >= part.size() &&
                      text->compare(text->size() - part.size(), part.size(), part) == 0);
    }
    if (method == "find") {
      if (args.size() != 1) fail(span, "Method 'find' expects 1 argument.");
      const std::string& needle = expectString(args[0], span, "The 'find' argument");
      std::size_t position = text->find(needle);
      if (position == std::string::npos) return makeInt(-1);
      std::size_t count = codepointCount(text->substr(0, position));
      return makeInt(static_cast<std::int64_t>(count));
    }
    if (method == "repeat") {
      if (args.size() != 1) fail(span, "Method 'repeat' expects 1 argument.");
      std::int64_t count = expectInt(args[0], span, "The count passed to 'repeat'");
      if (count < 0) fail(span, "The 'repeat' count must not be negative.");
      std::string out;
      for (std::int64_t i = 0; i < count; ++i) out += *text;
      return makeString(std::move(out));
    }
    if (method == "chars") {
      if (!args.empty()) fail(span, "Method 'chars' takes no arguments.");
      auto chars = std::make_shared<std::vector<Value>>();
      std::size_t index = 0;
      char32_t codepoint = 0;
      while (decodeUtf8(*text, index, codepoint)) chars->push_back(makeChar(codepoint));
      return makeArray(std::move(chars));
    }
    if (method == "try_to_int") {
      if (!args.empty()) fail(span, "Method 'try_to_int' takes no arguments.");
      std::int64_t value = 0;
      if (!parseInteger(*text, value)) return makeNone();
      return makeOptional(true, makeInt(value));
    }
    if (method == "try_to_float") {
      if (!args.empty()) fail(span, "Method 'try_to_float' takes no arguments.");
      double value = 0;
      if (!parseFloat(*text, value)) return makeNone();
      return makeOptional(true, makeFloat(value));
    }
    fail(span, "String has no method '" + method + "'.");
  }

  fail(span, "This value has no methods.");
}

// ---------------------------------------------------------------- conversions

Value Interpreter::convertNumber(const std::string& typeName, const Value& value, Span span) {
  auto* intValue = std::get_if<std::int64_t>(&value.data);
  auto* floatValue = std::get_if<double>(&value.data);
  if (!intValue && !floatValue) {
    fail(span, "'" + typeName + "' requires a numeric argument.");
  }

  const int bits = [&] {
    int value = 0;
    for (std::size_t i = 1; i < typeName.size(); ++i) {
      value = value * 10 + (typeName[i] - '0');
    }
    return value;
  }();

  if (typeName[0] == 'f') {
    double result = intValue ? static_cast<double>(*intValue) : *floatValue;
    if (bits == 32) result = static_cast<double>(static_cast<float>(result));
    return makeFloat(result);
  }

  const bool isSigned = typeName[0] == 'i';
  std::int64_t result = 0;
  if (intValue) {
    result = *intValue;
  } else {
    if (!std::isfinite(*floatValue)) {
      fail(span, "Cannot convert a non-finite float to '" + typeName + "'.");
    }
    if (*floatValue < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        *floatValue > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
      fail(span, "Float value is out of range for '" + typeName + "'.");
    }
    result = static_cast<std::int64_t>(*floatValue);
  }

  std::int64_t minimum = 0;
  std::int64_t maximum = 0;
  if (isSigned) {
    minimum = bits == 64 ? std::numeric_limits<std::int64_t>::min()
                         : -(std::int64_t{1} << (bits - 1));
    maximum = bits == 64 ? std::numeric_limits<std::int64_t>::max()
                         : (std::int64_t{1} << (bits - 1)) - 1;
  } else {
    minimum = 0;
    maximum = bits == 64 ? std::numeric_limits<std::int64_t>::max()
                         : (std::int64_t{1} << bits) - 1;
  }

  if (result < minimum || result > maximum) {
    fail(span, "Value " + std::to_string(result) + " is out of range for '" + typeName + "'.");
  }
  return makeInt(result);
}

// ---------------------------------------------------------------- helpers

std::int64_t Interpreter::expectInt(const Value& value, Span span, const std::string& what) {
  if (auto* result = std::get_if<std::int64_t>(&value.data)) return *result;
  fail(span, what + " must be an integer.");
}

double Interpreter::expectFloat(const Value& value, Span span, const std::string& what) {
  if (auto* result = std::get_if<double>(&value.data)) return *result;
  if (auto* integer = std::get_if<std::int64_t>(&value.data)) {
    return static_cast<double>(*integer);
  }
  fail(span, what + " must be a number.");
}

bool Interpreter::expectBool(const Value& value, Span span, const std::string& what) {
  if (auto* result = std::get_if<bool>(&value.data)) return *result;
  fail(span, what + " must be bool.");
}

char32_t Interpreter::expectChar(const Value& value, Span span, const std::string& what) {
  if (auto* result = std::get_if<char32_t>(&value.data)) return *result;
  fail(span, what + " must be a character.");
}

const std::string& Interpreter::expectString(const Value& value, Span span,
                                             const std::string& what) {
  if (auto* result = std::get_if<std::string>(&value.data)) return *result;
  fail(span, what + " must be a string.");
}

ArrayPtr Interpreter::expectArray(const Value& value, Span span, const std::string& what) {
  if (auto* result = std::get_if<ArrayPtr>(&value.data)) return *result;
  fail(span, what + " must be an array.");
}

}  // namespace a
