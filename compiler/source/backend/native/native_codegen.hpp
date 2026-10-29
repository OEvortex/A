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

// Compiles via the native C++ backend and executes the resulting program
// directly, streaming stdout/stderr. Returns the program's exit code, or -1
// when the program could not be compiled by the native emitter.
int runNativeBinary(const ModuleGraph& graph, const SemaResult& sema,
                    std::istream& in, std::ostream& out, std::ostream& err);

// Returns true if the AST only contains constructs the native C++ emitter
// compiles correctly, so `a run` can execute such programs at native build
// speed. Anything else falls back to the tree-walking interpreter.
bool isNativeSupported(const ModuleGraph& graph);

}  // namespace a
