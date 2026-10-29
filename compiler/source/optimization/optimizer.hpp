#pragma once

#include "ir/ir.hpp"

namespace a {
namespace ir {

// Optimization statistics collected while a pass walks the program.
struct OptimizationStats {
  int constantsFolded = 0;
  int deadInstructions = 0;
  int deadBlocks = 0;
  int functionsInlined = 0;
  int loopsHoisted = 0;

  void merge(const OptimizationStats& other) {
    constantsFolded += other.constantsFolded;
    deadInstructions += other.deadInstructions;
    deadBlocks += other.deadBlocks;
    functionsInlined += other.functionsInlined;
    loopsHoisted += other.loopsHoisted;
  }
};

// Constant folding and propagation across binaries, unaries, selects, phis,
// and string concatenation. Runs to a fixpoint.
OptimizationStats constantFold(Program& program);

// Removes unreachable blocks and pure instructions whose results are unused.
OptimizationStats deadCodeElimination(Program& program);

// Inlines single-block functions whose bodies contain only pure instructions.
OptimizationStats inlineFunctions(Program& program);

// Hoists loop-invariant pure instructions into natural-loop preheaders.
OptimizationStats loopInvariantCodeMotion(Program& program);

// Runs all passes until the program stops changing (bounded).
OptimizationStats runAll(Program& program);

}  // namespace ir
}  // namespace a
