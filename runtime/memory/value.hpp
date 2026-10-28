#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace a {

struct Value;
struct LambdaExpr;
struct FnDecl;

using ArrayPtr = std::shared_ptr<std::vector<Value>>;

struct StructObj;
using StructPtr = std::shared_ptr<StructObj>;

struct OptionalValue;
using OptionalPtr = std::shared_ptr<OptionalValue>;

struct TupleValue;
using TuplePtr = std::shared_ptr<TupleValue>;

struct Closure;
using ClosurePtr = std::shared_ptr<Closure>;

struct RangeValue {
  std::int64_t start = 0;
  std::int64_t end = 0;
};

struct Value {
  std::variant<std::monostate, bool, std::int64_t, double, char32_t, std::string,
               ArrayPtr, StructPtr, RangeValue, OptionalPtr, TuplePtr, ClosurePtr>
      data;
};

struct StructObj {
  std::string typeName;
  std::vector<std::pair<std::string, Value>> fields;
};

struct OptionalValue {
  bool hasValue = false;
  Value value;
};

struct TupleValue {
  std::vector<Value> items;
};

struct Closure {
  const LambdaExpr* decl = nullptr;  // lambda body, or null for a named function
  const FnDecl* fn = nullptr;        // named function referenced as a value
  std::unordered_map<std::string, Value> captured;
};

std::string formatValue(const Value& value, bool quoteStrings);
std::string utf8Encode(char32_t codepoint);

}  // namespace a
