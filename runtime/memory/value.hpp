#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "collections/transparent_map.hpp"

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

struct ModuleObj;
using ModulePtr = std::shared_ptr<ModuleObj>;

struct NativeFunction;
using NativeFunctionPtr = std::shared_ptr<NativeFunction>;

struct EnumObj;
using EnumPtr = std::shared_ptr<EnumObj>;

struct RangeValue {
  std::int64_t start = 0;
  std::int64_t end = 0;
  std::int64_t step = 1;
};

struct Value {
  std::variant<std::monostate, bool, std::int64_t, double, char32_t, std::string,
               ArrayPtr, StructPtr, RangeValue, OptionalPtr, TuplePtr, ClosurePtr, ModulePtr,
               NativeFunctionPtr, EnumPtr>
      data;
};

struct StructObj {
  std::string typeName;
  std::vector<std::pair<std::string, Value>> fields;
};

struct ModuleObj {
  int id = -1;
  std::string nativeName;  // set for native modules
};

struct NativeFunction {
  std::string builtinName;
};

struct EnumObj {
  std::string name;  // qualified enum name, e.g. "color::Color"
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
  int moduleId = -1;                 // module whose globals the closure sees
  StringMap<Value> captured;
};

}  // namespace a
