#pragma once

#include <iosfwd>
#include <string>

#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"

namespace a {

// Emits an optimized C++ translation unit from the typed AST and standard library.
bool emitNativeCpp(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out);

// Builds a native executable from the module graph using g++ with -O3 optimization.
bool buildNativeBinary(const ModuleGraph& graph, const SemaResult& sema,
                       const std::string& outputPath, std::ostream& err);

}  // namespace a
