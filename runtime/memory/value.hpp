#pragma once

#include <cstddef>
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

// A non-owning window into another array's storage. Slicing an array with a
// step of one produces a view; element writes through the view are visible in
// the source, and the view keeps the source storage alive.
struct ArrayView {
  ArrayPtr storage;
  std::size_t offset = 0;
  std::size_t length = 0;
};
using ArrayViewPtr = std::shared_ptr<ArrayView>;

struct MapObj {
  std::vector<std::pair<Value, Value>> entries;  // insertion order; unique keys
};
using MapPtr = std::shared_ptr<MapObj>;

struct SetObj {
  std::vector<Value> elements;  // insertion order; unique elements
};
using SetPtr = std::shared_ptr<SetObj>;

struct StructObj;
using StructPtr = std::shared_ptr<StructObj>;

struct OptionalValue;
using OptionalPtr = std::shared_ptr<OptionalValue>;

struct ResultValue;
using ResultPtr = std::shared_ptr<ResultValue>;

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
                ArrayPtr, MapPtr, SetPtr, ArrayViewPtr, StructPtr, RangeValue, OptionalPtr,
               ResultPtr, TuplePtr, ClosurePtr, ModulePtr, NativeFunctionPtr, EnumPtr>
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

struct ResultValue {
  bool ok = false;
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
