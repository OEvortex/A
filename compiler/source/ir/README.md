# Intermediate representation

`lowerProgram` turns an analyzed module graph into typed SSA IR with
control-flow basic blocks. Each function has value ids, explicit phis, and a
terminator per block.

- `Op` enumerates instructions: constants, `Param`, `Binary`, `Unary`,
  `Select`, `Copy`, calls, indexing/slicing, aggregate literals, iteration
  pseudo-ops, stores, and `Opaque` for constructs the core IR cannot represent.
- Loop headers receive phis for every variable assigned in the body before the
  body is lowered; join blocks materialize phis once all incoming edges are
  known.
- `printFunction` / `printProgram` render a textual form; the CLI exposes it as
  `a ir` (add `--opt` to run `optimization/` first).

Types come from `Sema`; literals with unconstrained types default to `i64` and
`f64` through `defaultType`. `and`/`or` are lowered as strict binaries, so the
IR models data flow only.
