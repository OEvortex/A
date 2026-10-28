#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace a {

// Hash for string-keyed tables that also accepts string_view lookups, so
// name resolution can search without allocating a temporary std::string.
struct TransparentStringHash {
  using is_transparent = void;

  std::size_t operator()(std::string_view text) const noexcept {
    return std::hash<std::string_view>{}(text);
  }
};

template <class Value>
using StringMap =
    std::unordered_map<std::string, Value, TransparentStringHash, std::equal_to<>>;

}  // namespace a
