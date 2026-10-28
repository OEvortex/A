#pragma once

// Umbrella header for embedding the A toolchain.

#include "diagnostics/diagnostics.hpp"
#include "lexer/source.hpp"
#include "lexer/span.hpp"
#include "lexer/token.hpp"
#include "driver/driver.hpp"
#include "ast/ast.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "backend/interpreter.hpp"
#include "memory/value.hpp"
#include "type-checker/sema.hpp"
#include "type-checker/types.hpp"
#include "driver/version.hpp"
