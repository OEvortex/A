# Performance Roadmap: Toward C++ Parity

This document is the engineering plan for closing the performance gap between
the A reference toolchain and optimized C++. It records the v0.3 baseline, the
source of the gap, the target definition, and the ordered phases that get there.

The short version: **A cannot match C++ while it executes programs with the
tree-walking interpreter.** The plan therefore moves execution through an
optimized bytecode VM (for `a run`) and a typed native backend (for `a build`),
with measurable gates at every phase.

Related reading:

- [`Plan.md`](../../Plan.md) sections 10 (optimization), 19 (benchmarking), and
  22 (milestones).
- [`docs/compiler-design`](../compiler-design/README.md) for the current
  pipeline.
- [`benchmarks/README.md`](../../benchmarks/README.md) for the suites.

---

## 1. Baseline (v0.3.0)

Measured 2026-10-28 on:

| Component | Version / detail |
| --- | --- |
| CPU | AMD Ryzen 5 7520U (8 logical cores) |
| OS | Linux x86-64 |
| A | v0.3.0, Release build (`build/a`), LTO on |
| C++ | g++ 13.3.0, `-O2` and `-O3`, no `-march=native`, no PGO |
| Python | CPython 3.13.14 |

### 1.1 Compute time per single workload run

A time is wall time minus its measured no-op startup (3.8 ms). Python and C++
are in-process timers. This removes process launch and interpreter init from
all three.

| Benchmark | C++ `-O2` | C++ `-O3` | Python | A | A vs C++ `-O2` |
| --- | ---: | ---: | ---: | ---: | ---: |
| `arithmetic/sum` (200k iterations) | 0.103 ms | 0.078 ms | 12.36 ms | 150.3 ms | **1,460× slower** |
| `collections/pipeline` (5k elements) | 0.013 ms | 0.012 ms | 0.55 ms | 14.1 ms | **1,050× slower** |
| `memory/strings` (100k `str()` + join) | 3.61 ms | 3.54 ms | 13.64 ms | 145.1 ms | **40× slower** |
| `numerical/sqrt` (50k `sqrt`) | 0.545 ms | 0.547 ms | 6.65 ms | 153.5 ms | **282× slower** |

### 1.2 Whole-process wall time (best of 7)

| Benchmark | C++ `-O2` | Python | A |
| --- | ---: | ---: | ---: |
| `arithmetic/sum` | 3.3 ms | 65.7 ms | 154.1 ms |
| `collections/pipeline` | 3.1 ms | 39.0 ms | 17.9 ms |
| `memory/strings` | 7.1 ms | 60.4 ms | 148.9 ms |
| `numerical/sqrt` | 3.9 ms | 53.4 ms | 157.3 ms |
| Startup / spawn | 1.4 ms | 42.9 ms | 3.8 ms |

A's only end-to-end win is `collections`, and only because Python pays ~39 ms
more startup; the compute itself is 26× slower than Python and ~1,000× slower
than C++.

Reproduce with:

```sh
RUNS=7 tools/benchmark-runner/run.sh build/a
python3 tools/benchmark-runner/compare.py --runs 7
```

---

## 2. Where the time goes

The gap is structural, not a missing compiler flag. Each A arithmetic iteration
costs roughly 470 ns against ~0.5 ns for C++. The budget is consumed by:

1. **RTTI dispatch per node.** `Interpreter::exec` and `Interpreter::eval`
   identify nodes with long `dynamic_cast` chains
   (`compiler/source/backend/interpreter.cpp:286+`). Every statement and
   expression pays at least one RTTI test.
2. **String-keyed environment lookups.** `Env::find` walks a parent chain and
   hashes a `std::string_view` for every variable read
   (`interpreter.hpp:35-46`). A loop variable is re-hashed on each iteration.
3. **Boxed values.** `Value` is a `std::variant` holding `int64`, `double`,
   `std::string`, and `shared_ptr` alternatives (`runtime/memory/value.hpp:48`).
   Every arithmetic result constructs a new `Value`; aggregates copy atomically
   refcounted pointers.
4. **String operator dispatch.** `applyBinary` compares `std::string` operator
   names (`interpreter.cpp:1091+`) before doing arithmetic, on top of checked
   overflow intrinsics.
5. **No unboxing or specialization.** The type checker knows every expression's
   type, but the interpreter re-derives it dynamically and keeps all locals in
   generic cells.
6. **Whole-program work per invocation.** Each `a run` re-lexes, re-parses, and
   re-typechecks the source (part of the 3.8 ms startup). This is small next to
   the compute gap but blocks competitive startup for compiled binaries.

The `memory` benchmark has the narrowest gap because C++ there is
allocation-bound (`std::to_string`, string vector, join), not dispatch-bound.

---

## 3. What "match C++" means

Performance claims must be falsifiable, so parity is defined per workload class
against g++ `-O2` with equivalent algorithms:

| Workload class | Target (A native / C++ `-O2`) | Hard gate |
| --- | --- | --- |
| Scalar integer/float loops, reductions | ≤ 1.10× | ≤ 1.25× |
| Numeric kernels (`sqrt`, transcendental, matmul) | ≤ 1.15× | ≤ 1.30× |
| Array/collection traversal and pipelines | ≤ 1.25× | ≤ 1.50× |
| Allocation/string-formatting workloads | ≤ 1.40× | ≤ 1.75× |
| Process startup (native binary) | ≤ 2 ms | ≤ 3 ms |

Rules:

- Both sides use the same algorithm; no hand-specialized A code against naive
  C++.
- Report compiler versions, flags, CPU, and run count. Use best-of-N for
  latency and median for noise; a result is only a regression if it exceeds 5%
  consistently across runs.
- Publish losses as well as wins. No blanket parity claim outside the measured
  matrix.
- `a run` (VM) and `a build` (native) are measured separately. Only `a build`
  is eligible for the C++ parity gates.

---

## 4. Strategy

Three tracks, in dependency order:

| Track | Deliverable | Expected speed vs v0.3 | Role |
| --- | --- | --- | --- |
| A | Optimized AST interpreter | 5–15× | Keeps `a run` usable during the transition |
| B | Register bytecode VM | 3–5× over Track A end state | Default `a run` executor, fast startup |
| C | Typed IR + LLVM AOT backend | 20–100× over Track B | The only path to C++ parity |

Track A is not sufficient and is not presented as the final answer. It pays for
itself because every fix (tagged AST, slot frames, unboxed values) is reused by
the VM and informs the IR design.

---

## 5. Phases

Effort is in engineering-weeks for one experienced compiler engineer; it is a
relative sizing, not a calendar commitment.

### P0 — Measurement infrastructure (done in this change)

Scope:

- Three-language equivalents of every benchmark.
- `compare.py` runner emitting A/Python/C++ tables, with compile step.
- Baseline recorded above.

Exit criteria: `compare.py` reproduces the tables above within noise on a
second machine.

### P1 — Front-end and dispatch fast path (2–3 weeks, 1.5–2.5×)

- Replace `dynamic_cast` chains with a tagged node kind (an `ExprKind`/
  `StmtKind` enum or a visitor) so dispatch is a switch jump table.
- Store binary/unary/assignment operators as interned enums in the AST and
  parser instead of `std::string`.
- Intern identifiers once (name → `SymbolId`) during name resolution; reuse the
  id in the AST and environment keys.
- Fold literals and constant expressions in the type checker; reject
  obviously dead branches before execution.
- Add a `--profile` counter mode (node visits, env lookups, allocations).

Exit criteria: `arithmetic` ≥ 1.5× faster, `collections` ≥ 1.5×, all tests
green, no semantic change.

### P2 — Value and allocation model (4–6 weeks, 1.3–2×)

- Replace `std::variant` with a 16-byte tagged value: inline `int64`/`double`/
  `bool`/`char32`, and an index/pointer for heap kinds. No atomic refcounts in
  the hot path.
- Give `Value` move-only fast paths and avoid copying in `eval` returns.
- Add small-string optimization (≤ 15 bytes inline) and an arena for
  short-lived aggregates.
- Introduce copy elision and scalar replacement in the AST interpreter for
  locals whose address never escapes.
- Split checked arithmetic behind a branch-predicted slow path; keep semantics
  identical.

Exit criteria: `memory/strings` ≥ 1.6× faster, `arithmetic` ≥ 2× cumulative,
peak RSS not worse than baseline +10%.

### P3 — Slot-resolved frames (3–4 weeks, 2–3×)

- Extend `Sema` to assign every local a `(scopeDepth, slotIndex)` and every
  function a stable `FunctionId`.
- Replace `Env`/`StringMap<Binding>` with a contiguous `ActivationRecord`
  (`std::vector<Cell>`); identifier reads become an array index.
- Resolve globals and imports to slot indices per module.
- Bind call arguments directly into the callee frame; remove per-call hash-map
  construction.
- Cache method/struct member lookup by id rather than name.

Exit criteria: `arithmetic` ≥ 3× cumulative, `collections` ≥ 3×, no linear env
walks left in profiles.

### P4 — Register bytecode VM (6–8 weeks, 3–5× over P3)

- Define a typed, fixed-width instruction set (locals, constants, arithmetic,
  branches, calls, field/index access, iteration).
- Compile the AST to bytecode once per file; cache compiled units by content
  hash for `a run`.
- Use register (not stack) operands and superinstructions for common pairs
  (load+add+store, compare+branch).
- Implement constant pools, inline caches for property/method access, and
  computed-goto or a switch dispatcher (benchmark both).
- Keep a debug mode that maps bytecode offsets to source spans for diagnostics
  and `defer`/error semantics.

Exit criteria: VM is the default `a run` executor, `arithmetic` ≥ 10×
cumulative, startup ≤ 5 ms for cached units, full integration suite green.

### P5 — Typed IR and LLVM backend (10–14 weeks, native parity)

- Define a typed SSA IR (`compiler/source/ir`) with explicit control flow,
  aggregate layout, and calling convention.
- Lower from the typed AST after monomorphization; retain source spans for
  diagnostics and debug info.
- Emit LLVM IR; link the A runtime (strings, arrays, I/O, errors) as a static
  library.
- Implement a value/ownership model with deterministic destruction; represent
  `defer` as structured cleanup on every exit edge.
- Keep overflow/bounds checks but structure them for branch prediction and
  later elimination.
- Build `a build program.a -o program` and make it the primary release path.

Exit criteria: `arithmetic` and `numerical` within 1.25× of C++ `-O2`;
`collections` within 1.5×; `memory` within 1.75×; startup ≤ 3 ms; all
correctness suites pass in both interpreted and compiled modes.

### P6 — Optimizations and runtime quality (6–8 weeks, closes remaining gap)

- Inlining with size heuristics, LICM, strength reduction, unrolling.
- Escape analysis, allocation sinking, stack promotion for non-escaping
  aggregates.
- Bounds-check elimination when the index range is provable.
- Monomorphization of generic collections and functions; devirtualization of
  method calls.
- String builder/interning aligned with the `memory` benchmark's real cost.
- Optional PGO and BOLT workflows for release builds.
- SIMD autovectorization for numeric kernels; verify with `-Rpass`-style
  reports.

Exit criteria: all four suites inside the hard gates in §3, zero regressions in
the published matrix, allocation counts within 1.3× of C++ on `memory`.

### P7 — Continuous performance gates

- CI job builds `a`, compiles the C++ equivalents, and runs `compare.py` on a
  pinned machine.
- Store results as JSON artifacts; fail on >5% regression against the previous
  release.
- Publish a per-release performance report, including failures.

---

## 6. Concrete work items by area

| Area | Files | Change |
| --- | --- | --- |
| AST dispatch | `compiler/source/ast/ast.hpp`, `backend/interpreter.cpp` | Node kinds / visitor; remove `dynamic_cast` chains |
| Operators | `lexer/token.hpp`, `parser/parser.cpp`, `ast/ast.hpp` | `Op` enum in parser and AST |
| Names | `name-resolution/module_loader.*`, `type-checker/sema.*` | `SymbolId` interning, export tables keyed by id |
| Scopes | `type-checker/sema.*`, `backend/interpreter.*` | Slot assignment, `ActivationRecord` |
| Values | `runtime/memory/value.hpp`, `backend/interpreter.cpp` | Tagged 16-byte value, SSO, arena |
| IR | `compiler/source/ir/` (new) | Typed SSA, verifier, printer |
| Backend | `compiler/source/backend/` (new `bytecode.*`, `llvm_backend.*`) | VM and LLVM emission |
| CLI | `compiler/source/cli/main.cpp`, `driver/driver.cpp` | `a build`, `--emit-ir`, `--emit-bytecode`, `--profile` |
| Benchmarks | `benchmarks/`, `tools/benchmark-runner/compare.py` | Equivalents, JSON output, gates |

---

## 7. Verification

- Correctness first: the integration suite must pass at every phase; the
  interpreter and VM/backend run the same golden tests.
- Differential testing: random programs produce identical output under the
  interpreter, VM, and native backend.
- Optimization validation: each pass has a debug verifier and a test pair
  showing transformed vs untransformed behavior.
- Performance validation: `compare.py` output is the only accepted evidence;
  screenshots and ad-hoc timings are not.

---

## 8. Risks and non-goals

**Risks**

- **Semantic drift.** Checked arithmetic, `defer`, reference-valued arrays, and
  closure capture rules must survive optimization. Mitigation: differential
  tests and a semantics-first IR.
- **Benchmark gaming.** Optimizing only the four published programs would be a
  false result. Mitigation: add real-world suites before claiming parity.
- **Locale/allocator variance.** `memory/strings` depends on `std::to_string`
  and allocation behavior. Mitigation: publish allocation counts alongside time.
- **Scope.** The native backend is the largest single work item; it should not
  block VM work that benefits `a run` immediately.

**Non-goals**

- Matching hand-tuned C++ with `-march=native` on every kernel.
- Matching C++ compile times.
- Guaranteeing parity for code dominated by I/O, syscalls, or dynamic dispatch
  the language cannot devirtualize.
- Keeping the tree-walking interpreter as the release execution path.

---

## 9. Definition of done

`docs/performance/README.md` is satisfied when all of the following hold:

- [ ] `a build` emits native executables with no interpreter in the hot path.
- [ ] All four benchmark classes meet the hard gates in §3 on two machines.
- [ ] Startup ≤ 3 ms for a native binary on the reference machine.
- [ ] The integration suite passes in interpreted and compiled modes.
- [ ] Results are reproducible from a clean checkout with documented commands.
- [ ] `Plan.md` §19.6 and the release performance report link to the latest run.
