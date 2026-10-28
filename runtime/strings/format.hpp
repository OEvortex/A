#pragma once

#include <string>

#include "memory/value.hpp"

namespace a {

// Renders a value for display. Strings are quoted when `quoteStrings` is true
// (used when they appear inside containers) and raw otherwise.
std::string formatValue(const Value& value, bool quoteStrings);

}  // namespace a
