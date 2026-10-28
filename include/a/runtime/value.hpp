#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace a {

struct Value;
using ArrayPtr = std::shared_ptr<std::vector<Value>>;

struct StructObj;
using StructPtr = std::shared_ptr<StructObj>;

struct RangeValue {
  std::int64_t start = 0;
  std::int64_t end = 0;
};

struct Value {
  std::variant<std::monostate, bool, std::int64_t, double, char32_t, std::string,
               ArrayPtr, StructPtr, RangeValue>
      data;
};

struct StructObj {
  std::string typeName;
  std::vector<std::pair<std::string, Value>> fields;
};

std::string formatValue(const Value& value, bool quoteStrings);
std::string utf8Encode(char32_t codepoint);

}  // namespace a
