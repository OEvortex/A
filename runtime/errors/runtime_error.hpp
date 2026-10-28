#pragma once

#include <string>

#include "lexer/span.hpp"

namespace a {

// Thrown by the interpreter for recoverable runtime failures. The driver
// renders it through the diagnostic sink and exits with code 2.
struct RuntimeError {
  Span span;
  std::string message;
  std::string help;
};

}  // namespace a
