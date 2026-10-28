#pragma once

#include <iosfwd>
#include <string>

#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"

namespace a {

// Emits standard textual LLVM IR (.ll) from the typed AST and module graph
bool emitLLVMIR(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out);

// Emits standard GNU x86-64 assembly (.s) directly from the module graph
bool emitX86Assembly(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out);

// Compiles the module graph to a native binary using LLVM IR / x86-64 native assembly & linker
bool buildLLVMModule(const ModuleGraph& graph, const SemaResult& sema,
                     const std::string& outputPath, std::ostream& err);

// Compiles via LLVM/native pipeline and executes the resulting program directly.
int runLLVMModule(const ModuleGraph& graph, const SemaResult& sema,
                  std::istream& in, std::ostream& out, std::ostream& err);

}  // namespace a
