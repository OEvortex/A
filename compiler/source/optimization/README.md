# Optimizations

IR passes over the typed SSA program in `ir/`. All passes return
`OptimizationStats` and are exposed through `optimization.hpp`:

- `constantFold` — folds integer/float/boolean/string operations, copies,
  selects, and phis; iterates to a fixpoint.
- `deadCodeElimination` — removes blocks unreachable from entry (rewriting phi
  incoming edges) and pure instructions whose values are unused.
- `inlineFunctions` — inlines single-block functions built only from pure
  instructions, substituting arguments for parameters.
- `loopInvariantCodeMotion` — hoists pure instructions out of natural loops
  (unique preheader required) using computed dominators.
- `runAll` — runs the passes to a fixpoint (bounded at eight rounds).

`a ir --opt file.a` prints the result. Constant folding skips integer
division/remainder by zero and out-of-range shifts; LICM skips division and
remainder so a never-executed loop cannot introduce a trap.
