#include "a/runtime/interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace a {
namespace {

constexpr int kMaxCallDepth = 2000;

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

Value makeRange(std::int64_t start, std::int64_t end) {
  Value out;
  out.data = RangeValue{start, end};
  return out;
}

bool decodeUtf8(const std::string& text, std::size_t& index, char32_t& out) {
  if (index >= text.size()) return false;
  const unsigned char first = static_cast<unsigned char>(text[index]);
  if (first < 0x80) {
    out = first;
    ++index;
    return true;
  }
  int extra = 0;
  char32_t value = 0;
  if ((first & 0xE0) == 0xC0) {
    extra = 1;
    value = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    extra = 2;
    value = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    extra = 3;
    value = first & 0x07;
  } else {
    out = 0xFFFD;
    ++index;
    return true;
  }
  if (index + static_cast<std::size_t>(extra) >= text.size()) {
    out = 0xFFFD;
    ++index;
    return true;
  }
  for (int i = 1; i <= extra; ++i) {
    const unsigned char byte = static_cast<unsigned char>(text[index + static_cast<std::size_t>(i)]);
    if ((byte & 0xC0) != 0x80) {
      out = 0xFFFD;
      ++index;
      return true;
    }
    value = (value << 6) | (byte & 0x3F);
  }
  index += static_cast<std::size_t>(extra) + 1;
  out = value;
  return true;
}

std::size_t codepointCount(const std::string& text) {
  std::size_t count = 0;
  std::size_t index = 0;
  char32_t codepoint = 0;
  while (decodeUtf8(text, index, codepoint)) ++count;
  return count;
}

std::string quoteString(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c); break;
    }
  }
  out.push_back('"');
  return out;
}

std::string formatDouble(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  std::string text(buffer, result.ptr);
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
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

std::string utf8Encode(char32_t codepoint) {
  std::string out;
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return out;
}

std::string formatValue(const Value& value, bool quoteStrings) {
  if (std::holds_alternative<std::monostate>(value.data)) return "void";
  if (auto* b = std::get_if<bool>(&value.data)) return *b ? "true" : "false";
  if (auto* i = std::get_if<std::int64_t>(&value.data)) return std::to_string(*i);
  if (auto* d = std::get_if<double>(&value.data)) return formatDouble(*d);
  if (auto* c = std::get_if<char32_t>(&value.data)) return utf8Encode(*c);
  if (auto* s = std::get_if<std::string>(&value.data)) {
    return quoteStrings ? quoteString(*s) : *s;
  }
  if (auto* array = std::get_if<ArrayPtr>(&value.data)) {
    std::string out = "[";
    for (std::size_t i = 0; i < (*array)->size(); ++i) {
      if (i) out += ", ";
      out += formatValue((**array)[i], true);
    }
    out += "]";
    return out;
  }
  if (auto* object = std::get_if<StructPtr>(&value.data)) {
    std::string out = (*object)->typeName + "(";
    for (std::size_t i = 0; i < (*object)->fields.size(); ++i) {
      if (i) out += ", ";
      out += (*object)->fields[i].first + "=" + formatValue((*object)->fields[i].second, true);
    }
    out += ")";
    return out;
  }
  if (auto* range = std::get_if<RangeValue>(&value.data)) {
    return std::to_string(range->start) + ".." + std::to_string(range->end);
  }
  return "void";
}

Interpreter::Interpreter(const Program& program, const SemaResult& result,
                         DiagnosticSink& sink, std::istream& in, std::ostream& out,
                         std::ostream& err)
    : program_(program), sema_(result), sink_(sink), in_(in), out_(out), err_(err) {}

void Interpreter::fail(Span span, std::string message, std::string help) {
  throw RuntimeError{span, std::move(message), std::move(help)};
}

int Interpreter::run() {
  Env global;
  globalEnv_ = &global;
  try {
    execStatements(program_.statements, global);
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
  return 0;
}

// ---------------------------------------------------------------- execution

void Interpreter::execStatements(const std::vector<StmtPtr>& statements, Env& env) {
  std::vector<const Stmt*> defers;
  std::exception_ptr pending;

  try {
    for (const auto& statement : statements) {
      if (auto* defer = dynamic_cast<const DeferStmt*>(statement.get())) {
        defers.push_back(defer->body.get());
        continue;
      }
      exec(*statement, env);
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
  if (auto* node = dynamic_cast<const LetStmt*>(&stmt)) {
    Binding binding;
    binding.value = eval(*node->init, env);
    binding.mutableBinding = node->mutableBinding;
    env.vars[node->name] = std::move(binding);
    return;
  }

  if (auto* node = dynamic_cast<const AssignStmt*>(&stmt)) {
    LValue target = resolveLValue(*node->target, env);
    if (node->op == "=") {
      writeLValue(target, eval(*node->value, env));
    } else {
      Value left = readLValue(target);
      Value right = eval(*node->value, env);
      std::string baseOp = node->op.substr(0, node->op.size() - 1);
      writeLValue(target, applyBinary(baseOp, left, right, node->opSpan));
    }
    return;
  }

  if (auto* node = dynamic_cast<const ExprStmt*>(&stmt)) {
    eval(*node->expr, env);
    return;
  }

  if (auto* node = dynamic_cast<const IfStmt*>(&stmt)) {
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

  if (auto* node = dynamic_cast<const WhileStmt*>(&stmt)) {
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
    }
    return;
  }

  if (auto* node = dynamic_cast<const ForStmt*>(&stmt)) {
    Value iterable = eval(*node->iterable, env);

    auto runIteration = [&](Value element) {
      Env child;
      child.parent = &env;
      Binding binding;
      binding.value = std::move(element);
      binding.mutableBinding = false;
      child.vars[node->var] = std::move(binding);
      try {
        execStatements(node->body, child);
      } catch (BreakSignal&) {
        throw BreakSignal{};
      }
    };

    if (auto* range = std::get_if<RangeValue>(&iterable.data)) {
      for (std::int64_t i = range->start; i < range->end; ++i) {
        try {
          runIteration(makeInt(i));
        } catch (BreakSignal&) {
          break;
        } catch (ContinueSignal&) {
          // next iteration
        }
        if (i == std::numeric_limits<std::int64_t>::max()) break;
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
      }
      return;
    }

    fail(node->iterable->span, "This value cannot be iterated over.");
  }

  if (auto* node = dynamic_cast<const LoopStmt*>(&stmt)) {
    while (true) {
      try {
        execBlock(node->body, env);
      } catch (BreakSignal&) {
        break;
      } catch (ContinueSignal&) {
        continue;
      }
    }
    return;
  }

  if (auto* node = dynamic_cast<const ReturnStmt*>(&stmt)) {
    Value value = node->value ? eval(*node->value, env) : makeVoid();
    throw ReturnSignal{std::move(value)};
  }

  if (dynamic_cast<const BreakStmt*>(&stmt)) throw BreakSignal{};
  if (dynamic_cast<const ContinueStmt*>(&stmt)) throw ContinueSignal{};

  if (auto* node = dynamic_cast<const DeferStmt*>(&stmt)) {
    exec(*node->body, env);
    return;
  }

  // Function and struct declarations have no runtime effect.
}

// ---------------------------------------------------------------- lvalues

Interpreter::LValue Interpreter::resolveLValue(const Expr& target, Env& env) {
  LValue lvalue;

  if (auto* ident = dynamic_cast<const IdentExpr*>(&target)) {
    Binding* binding = env.find(ident->name);
    if (!binding) {
      Binding fresh;
      fresh.mutableBinding = false;
      env.vars[ident->name] = std::move(fresh);
      binding = &env.vars[ident->name];
    }
    lvalue.kind = LValue::Kind::Variable;
    lvalue.binding = binding;
    return lvalue;
  }

  if (auto* field = dynamic_cast<const FieldExpr*>(&target)) {
    Value objectValue = eval(*field->object, env);
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
    fail(field->fieldSpan, "Struct '" + lvalue.object->typeName + "' has no field '" +
                               field->field + "'.");
  }

  if (auto* index = dynamic_cast<const IndexExpr*>(&target)) {
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
    fail(span, "Array index " + std::to_string(index) + " is negative.",
         "Indices start at 0.");
  }
  if (static_cast<std::size_t>(index) >= length) {
    fail(span, "Array index " + std::to_string(index) + " is out of bounds for an array of length " +
                   std::to_string(length) + ".");
  }
  return index;
}

// ---------------------------------------------------------------- evaluation

Value Interpreter::eval(const Expr& expr, Env& env) {
  if (auto* node = dynamic_cast<const IntLitExpr*>(&expr)) return makeInt(node->value);
  if (auto* node = dynamic_cast<const FloatLitExpr*>(&expr)) return makeFloat(node->value);
  if (auto* node = dynamic_cast<const BoolLitExpr*>(&expr)) return makeBool(node->value);
  if (auto* node = dynamic_cast<const CharLitExpr*>(&expr)) return makeChar(node->value);
  if (auto* node = dynamic_cast<const StringLitExpr*>(&expr)) return evalString(*node, env);

  if (auto* node = dynamic_cast<const IdentExpr*>(&expr)) {
    Binding* binding = env.find(node->name);
    if (!binding) fail(node->span, "Undefined variable '" + node->name + "'.");
    return binding->value;
  }

  if (auto* node = dynamic_cast<const UnaryExpr*>(&expr)) {
    Value operand = eval(*node->operand, env);
    if (node->op == "not") {
      return makeBool(!expectBool(operand, node->span, "The 'not' operand"));
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

  if (auto* node = dynamic_cast<const BinaryExpr*>(&expr)) return evalBinary(*node, env);

  if (auto* node = dynamic_cast<const CallExpr*>(&expr)) return evalCall(*node, env);
  if (auto* node = dynamic_cast<const IndexExpr*>(&expr)) return evalIndex(*node, env);
  if (auto* node = dynamic_cast<const SliceExpr*>(&expr)) return evalSlice(*node, env);
  if (auto* node = dynamic_cast<const FieldExpr*>(&expr)) return evalField(*node, env);
  if (auto* node = dynamic_cast<const ArrayLitExpr*>(&expr)) return evalArray(*node, env);

  fail(expr.span, "Internal error: unsupported expression.");
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
  auto* array = std::get_if<ArrayPtr>(&object.data);
  if (!array) fail(expr.span, "Only arrays can be indexed.");
  Value indexValue = eval(*expr.index, env);
  std::int64_t index = expectInt(indexValue, expr.index->span, "The array index");
  std::size_t normalized = static_cast<std::size_t>(normalizeIndex(index, (*array)->size(), expr.span));
  return (**array)[normalized];
}

Value Interpreter::evalSlice(const SliceExpr& expr, Env& env) {
  Value object = eval(*expr.object, env);
  auto* array = std::get_if<ArrayPtr>(&object.data);
  if (!array) fail(expr.span, "Only arrays can be sliced.");

  std::int64_t length = static_cast<std::int64_t>((*array)->size());
  std::int64_t start = 0;
  std::int64_t end = length;
  if (expr.start) {
    start = expectInt(eval(*expr.start, env), expr.start->span, "The slice start");
  }
  if (expr.end) {
    end = expectInt(eval(*expr.end, env), expr.end->span, "The slice end");
  }
  if (start < 0 || end < 0) fail(expr.span, "Slice bounds cannot be negative.");
  if (start > end) fail(expr.span, "Slice start must not exceed the slice end.");
  if (start > length || end > length) {
    fail(expr.span, "Slice bounds are outside the array (length " + std::to_string(length) + ").");
  }

  auto result = std::make_shared<std::vector<Value>>((*array)->begin() + start,
                                                     (*array)->begin() + end);
  return makeArray(std::move(result));
}

Value Interpreter::evalField(const FieldExpr& expr, Env& env) {
  Value object = eval(*expr.object, env);
  auto* structValue = std::get_if<StructPtr>(&object.data);
  if (!structValue) fail(expr.span, "Only struct values have fields.");
  for (const auto& field : (*structValue)->fields) {
    if (field.first == expr.field) return field.second;
  }
  fail(expr.fieldSpan,
       "Struct '" + (*structValue)->typeName + "' has no field '" + expr.field + "'.");
}

Value Interpreter::evalBinary(const BinaryExpr& expr, Env& env) {
  if (expr.op == "and") {
    bool left = expectBool(eval(*expr.lhs, env), expr.lhs->span, "The left operand of 'and'");
    if (!left) return makeBool(false);
    return makeBool(expectBool(eval(*expr.rhs, env), expr.rhs->span, "The right operand of 'and'"));
  }
  if (expr.op == "or") {
    bool left = expectBool(eval(*expr.lhs, env), expr.lhs->span, "The left operand of 'or'");
    if (left) return makeBool(true);
    return makeBool(expectBool(eval(*expr.rhs, env), expr.rhs->span, "The right operand of 'or'"));
  }
  if (expr.op == "..") {
    std::int64_t start = expectInt(eval(*expr.lhs, env), expr.lhs->span, "The range start");
    std::int64_t end = expectInt(eval(*expr.rhs, env), expr.rhs->span, "The range end");
    return makeRange(start, end);
  }

  Value left = eval(*expr.lhs, env);
  Value right = eval(*expr.rhs, env);
  return applyBinary(expr.op, left, right, expr.opSpan);
}

Value Interpreter::applyBinary(const std::string& op, const Value& lhs, const Value& rhs,
                               Span span) {
  if (auto* left = std::get_if<std::int64_t>(&lhs.data)) {
    auto* right = std::get_if<std::int64_t>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between integer and non-integer.");
    std::int64_t result = 0;
    if (op == "+") {
      if (__builtin_add_overflow(*left, *right, &result)) fail(span, "Integer overflow in '+'.");
      return makeInt(result);
    }
    if (op == "-") {
      if (__builtin_sub_overflow(*left, *right, &result)) fail(span, "Integer overflow in '-'.");
      return makeInt(result);
    }
    if (op == "*") {
      if (__builtin_mul_overflow(*left, *right, &result)) fail(span, "Integer overflow in '*'.");
      return makeInt(result);
    }
    if (op == "/") {
      if (*right == 0) fail(span, "Division by zero.", "Check the divisor before dividing.");
      if (*left == std::numeric_limits<std::int64_t>::min() && *right == -1) {
        fail(span, "Integer overflow in '/'.");
      }
      return makeInt(*left / *right);
    }
    if (op == "%") {
      if (*right == 0) fail(span, "Remainder by zero.", "Check the divisor before dividing.");
      if (*right == -1) return makeInt(0);
      return makeInt(*left % *right);
    }
    if (op == "**") {
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
    if (op == "==") return makeBool(*left == *right);
    if (op == "!=") return makeBool(*left != *right);
    if (op == "<") return makeBool(*left < *right);
    if (op == ">") return makeBool(*left > *right);
    if (op == "<=") return makeBool(*left <= *right);
    if (op == ">=") return makeBool(*left >= *right);
    fail(span, "Operator '" + op + "' is not defined for integers.");
  }

  if (auto* left = std::get_if<double>(&lhs.data)) {
    auto* right = std::get_if<double>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between float and non-float.");
    if (op == "+") return makeFloat(*left + *right);
    if (op == "-") return makeFloat(*left - *right);
    if (op == "*") return makeFloat(*left * *right);
    if (op == "/") return makeFloat(*left / *right);
    if (op == "**") return makeFloat(std::pow(*left, *right));
    if (op == "==") return makeBool(*left == *right);
    if (op == "!=") return makeBool(*left != *right);
    if (op == "<") return makeBool(*left < *right);
    if (op == ">") return makeBool(*left > *right);
    if (op == "<=") return makeBool(*left <= *right);
    if (op == ">=") return makeBool(*left >= *right);
    fail(span, "Operator '" + op + "' is not defined for floats.");
  }

  if (auto* left = std::get_if<std::string>(&lhs.data)) {
    auto* right = std::get_if<std::string>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between string and non-string.");
    if (op == "+") return makeString(*left + *right);
    if (op == "==") return makeBool(*left == *right);
    if (op == "!=") return makeBool(*left != *right);
    if (op == "<") return makeBool(*left < *right);
    if (op == ">") return makeBool(*left > *right);
    if (op == "<=") return makeBool(*left <= *right);
    if (op == ">=") return makeBool(*left >= *right);
    fail(span, "Operator '" + op + "' is not defined for strings.");
  }

  if (auto* left = std::get_if<char32_t>(&lhs.data)) {
    auto* right = std::get_if<char32_t>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between characters.");
    if (op == "==") return makeBool(*left == *right);
    if (op == "!=") return makeBool(*left != *right);
    if (op == "<") return makeBool(*left < *right);
    if (op == ">") return makeBool(*left > *right);
    if (op == "<=") return makeBool(*left <= *right);
    if (op == ">=") return makeBool(*left >= *right);
    fail(span, "Operator '" + op + "' is not defined for characters.");
  }

  if (auto* left = std::get_if<bool>(&lhs.data)) {
    auto* right = std::get_if<bool>(&rhs.data);
    if (!right) fail(span, "Internal type mismatch between booleans.");
    if (op == "==") return makeBool(*left == *right);
    if (op == "!=") return makeBool(*left != *right);
    fail(span, "Operator '" + op + "' is not defined for bools.");
  }

  fail(span, "Operator '" + op + "' cannot be applied to these values.");
}

// ---------------------------------------------------------------- calls

Value Interpreter::evalCall(const CallExpr& expr, Env& env) {
  if (auto* ident = dynamic_cast<const IdentExpr*>(expr.callee.get())) {
    auto structIt = sema_.structs.find(ident->name);
    if (structIt != sema_.structs.end()) {
      return constructStruct(*structIt->second, expr.args, env, expr.span);
    }

    std::vector<Value> args;
    args.reserve(expr.args.size());
    for (const auto& arg : expr.args) args.push_back(eval(*arg.value, env));

    auto fnIt = sema_.functions.find(ident->name);
    if (fnIt != sema_.functions.end()) {
      return callFunction(*fnIt->second, std::move(args), expr.span);
    }
    return callBuiltin(ident->name, args, expr.span);
  }

  if (auto* field = dynamic_cast<const FieldExpr*>(expr.callee.get())) {
    Value object = eval(*field->object, env);
    std::vector<Value> args;
    args.reserve(expr.args.size());
    for (const auto& arg : expr.args) args.push_back(eval(*arg.value, env));

    if (auto* structValue = std::get_if<StructPtr>(&object.data)) {
      return callMethod(*structValue, field->field, std::move(args), expr.span);
    }
    return callBuiltinMethod(object, field->field, args, expr.span);
  }

  fail(expr.span, "This expression is not callable.");
}

Value Interpreter::callFunction(const FnDecl& decl, std::vector<Value> args, Span span) {
  if (args.size() != decl.params.size()) {
    fail(span, "Function '" + decl.name + "' expects " + std::to_string(decl.params.size()) +
                   " argument(s), found " + std::to_string(args.size()) + ".");
  }

  CallDepthGuard guard(callDepth_);
  if (callDepth_ > kMaxCallDepth) {
    fail(span, "Maximum call depth of " + std::to_string(kMaxCallDepth) + " exceeded.",
         "Check for infinite recursion.");
  }

  Env frame;
  frame.parent = globalEnv_;
  for (std::size_t i = 0; i < decl.params.size(); ++i) {
    Binding binding;
    binding.value = std::move(args[i]);
    binding.mutableBinding = false;
    frame.vars[decl.params[i].name] = std::move(binding);
  }

  try {
    execStatements(decl.body, frame);
  } catch (ReturnSignal& signal) {
    return std::move(signal.value);
  }
  return makeVoid();
}

Value Interpreter::callMethod(const StructPtr& object, const std::string& methodName,
                              std::vector<Value> args, Span span) {
  const StructDecl* decl = findStruct(object->typeName);
  if (!decl) fail(span, "Internal error: unknown struct '" + object->typeName + "'.");

  for (const auto& method : decl->methods) {
    if (method->name != methodName) continue;
    std::vector<Value> all;
    all.reserve(args.size() + 1);
    all.push_back(makeStructValue(object));
    for (auto& arg : args) all.push_back(std::move(arg));
    return callFunction(*method, std::move(all), span);
  }

  fail(span, "Struct '" + object->typeName + "' has no method '" + methodName + "'.");
}

Value Interpreter::constructStruct(const StructDecl& decl, const std::vector<CallArg>& args,
                                   Env& env, Span span) {
  std::vector<Value> values(decl.fields.size());
  std::vector<bool> provided(decl.fields.size(), false);

  std::size_t nextPositional = 0;
  for (const auto& arg : args) {
    if (arg.name.empty()) {
      if (nextPositional >= decl.fields.size()) {
        fail(span, "Too many positional arguments for struct '" + decl.name + "'.");
      }
      values[nextPositional] = eval(*arg.value, env);
      provided[nextPositional] = true;
      ++nextPositional;
      continue;
    }

    std::size_t index = decl.fields.size();
    for (std::size_t i = 0; i < decl.fields.size(); ++i) {
      if (decl.fields[i].name == arg.name) {
        index = i;
        break;
      }
    }
    if (index == decl.fields.size()) {
      fail(arg.nameSpan, "Struct '" + decl.name + "' has no field '" + arg.name + "'.");
    }
    values[index] = eval(*arg.value, env);
    provided[index] = true;
  }

  auto object = std::make_shared<StructObj>();
  object->typeName = decl.name;
  object->fields.reserve(decl.fields.size());
  for (std::size_t i = 0; i < decl.fields.size(); ++i) {
    if (!provided[i]) {
      fail(span, "Missing field '" + decl.fields[i].name + "' in struct '" + decl.name + "'.");
    }
    object->fields.emplace_back(decl.fields[i].name, std::move(values[i]));
  }
  return makeStructValue(std::move(object));
}

const StructDecl* Interpreter::findStruct(const std::string& name) const {
  auto found = sema_.structs.find(name);
  return found == sema_.structs.end() ? nullptr : found->second;
}

Value Interpreter::callBuiltin(const std::string& name, std::vector<Value>& args, Span span) {
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

  if (name == "i8" || name == "i16" || name == "i32" || name == "i64" || name == "u8" ||
      name == "u16" || name == "u32" || name == "u64" || name == "f32" || name == "f64") {
    if (args.size() != 1) fail(span, "'" + name + "' expects 1 argument.");
    return convertNumber(name, args[0], span);
  }

  fail(span, "Internal error: unknown builtin function '" + name + "'.");
}

Value Interpreter::callBuiltinMethod(const Value& object, const std::string& method,
                                     std::vector<Value>& args, Span span) {
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

  const int bits = std::stoi(typeName.substr(1));

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
