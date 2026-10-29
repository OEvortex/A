#pragma once

#include "diagnostics/diagnostics.hpp"
#include "name-resolution/module_loader.hpp"

namespace a {

// Flow-sensitive ownership checks layered on top of the type checker:
//
//   * `move(x)` transfers a value; using or moving `x` again before assigning
//     it a new value is an error.
//   * A unit-step slice (`items[1..4]`) borrows the array it reads. Changing
//     the length of the lender while the view is alive, or moving the lender,
//     is an error. Element writes remain allowed: views are windows by design.
//
// The analysis runs once per program after Sema and reports errors into the
// shared sink.
void analyzeOwnership(const ModuleGraph& graph, DiagnosticSink& sink);

}  // namespace a
