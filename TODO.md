# A lang — TODO List

Features listed in `Plan.md` that are **not yet implemented** in A lang.
Check off items as they are completed. Status is kept in sync with an audit
against the compiler (2026-10-29).

> ✅ = Implemented | ⬜ = Missing/Not implemented

---

## 1. Primitive Types

- [ ] `i128` — 128-bit signed integer (Plan §3.4)
- [ ] `u128` — 128-bit unsigned integer (Plan §3.4)
- [ ] `f16` — Low-precision float (Plan §14.1)
- [ ] `bf16` — Brain float 16 (Plan §14.1)

---

## 2. Syntax Features

- [x] Multiline comments `"""..."""` — a standalone raw string statement is
      evaluated and discarded (Plan §3.2)
- [x] Triple-quoted raw strings `"""..."""` — for SQL/docs/templates (Plan §3.14)
- [x] Open-ended ranges `items[1..]` — slicing without end (Plan §3.14)
- [x] `class` inheritance — `class User(Entity):` (Plan §8.4)

---

## 3. Operators

- [X] `++` / `--` — increment/decrement
- [ ] `pub` — public visibility keyword (Plan §8.5)
- [ ] `?` — error propagation over `Result` types (Plan §9.2).
      Optional unwrap with `!`, `?.`, and `??` is implemented; `?` after a
      `Result` call is not.

---

## 4. Type System

- [x] Generics `<T>` — `fn first<T>(items: [T]) -> T` (Plan §7.3)
- [ ] Generic code generation / monomorphization (Plan §7.4) — generic calls
      execute through the interpreter; LLVM/native codegen does not specialize them
- [ ] Interfaces / Traits — `trait Printable`, `impl ... for ...` (Plan §7.5)
- [ ] `Result<T, E>` — fallible return type syntax (Plan §9.1)
- [x] Type aliases — `type Kilograms = f64` (Plan §7.8)
- [ ] Algebraic enums with payloads — `enum Result<T, E>: Ok(value: T)` (Plan §7.6)

---

## 5. Arrays & Collections

- [ ] Fixed-size arrays — `numbers: i32[5] = [...]` (Plan §3.10)
- [ ] Multidimensional arrays (Plan §3.10)
- [ ] Views over existing storage (Plan §3.10)
- [ ] Unchecked indexing — unsafe interface (Plan §3.10)
- [ ] Map (`{k: v}`) and set (`{1, 2}`) literals — rejected with a clear
      diagnostic because no runtime value representation exists yet

---

## 6. Object-Oriented Programming

- [x] Class inheritance (Plan §8.4)
- [ ] `pub` visibility modifier (Plan §8.5)

---

## 7. Error Handling

- [ ] Exhaustive match checking — compiler verifies all cases (Plan §9.3)
- [ ] Panic recovery model — defined behavior and cleanup guarantees (Plan §9.4)

---

## 8. Standard Library

- [ ] Networking — TCP/UDP sockets, DNS, HTTP client/server, TLS (Plan §13.6)
- [ ] JSON serialization — structured serialization (Plan §13.7)
- [ ] Binary serialization (Plan §13.7)
- [ ] Property-based testing support (Plan §13.8)
- [ ] Structured logging (Plan §13.11)
- [ ] Metrics emission — Prometheus/OpenTelemetry compatible (Plan §13.11)
- [ ] Distributed tracing (Plan §13.11)

---

## 9. AI & Numerical Computing

- [ ] Tensor library — multidimensional tensors, strides, broadcasting (Plan §14.2)
- [ ] GPU interoperability — device memory, kernel launch, transfers (Plan §14.4)
- [ ] AI inference via FFI (Plan §14.5)

---

## 10. SIMD & Hardware Performance

- [ ] SIMD vector operations — x86-64, ARM NEON (Plan §11.1)
- [ ] Auto-vectorization — optimizer identifies vectorizable loops (Plan §11.2)
- [ ] Memory alignment — defined alignment requirements (Plan §11.3)
- [ ] Atomic operations — `AtomicBool`, `AtomicI64`, `AtomicUsize` (Plan §11.4)

---

## 11. Concurrency & Parallelism

- [ ] `spawn` — OS threads / coroutines (Plan §12.1)
- [ ] `scope:` — structured concurrency scopes (Plan §12.1)
- [ ] `Mutex<T>` / `RwLock<T>` — lock-data coupling (Plan §12.1)
- [ ] `channel<T>(capacity)` — thread-safe channels (Plan §12.1)
- [ ] `par_for` — parallel loops (Plan §12.3)
- [ ] `par_iter().map().collect()` — parallel map with work-stealing (Plan §12.4)
- [ ] `Send` / `Sync` traits — compile-time data-race prevention (Plan §12.5)

---

## 12. Package & Module System

- [ ] Package manager — dependency declarations, version constraints, lockfiles (Plan §15.2)
- [ ] Dependency checksums / integrity verification (Plan §15.2, §15.4)
- [ ] `a bindgen` — automatic C header binding generation (Plan §13.10)
- [ ] Build configuration — compiler flags, target architectures (Plan §15.3)

---

## 13. CLI Toolchain

- [x] `a check` — compilation checks without producing executable (Plan §16)
- [x] `a fmt` — source code formatter (Plan §16)
- [x] `a doc` — documentation generator (Plan §16)
- [x] `a test` — test runner for `fn test_*()` (Plan §16)
- [ ] `a package` — package manager commands (Plan §16)

---

## 14. Compiler Backend & IR

- [ ] Intermediate Representation (IR) — typed IR, SSA, control-flow blocks (Plan §5)
- [x] Native code generation — LLVM backend and native C++ backend (Plan §4.7, §10)
- [ ] Optimization passes — constant folding, DCE, inlining, loop optimization (Plan §10)
- [ ] Ownership & lifetime analysis — borrow checker, move semantics (Plan §6)
- [ ] Custom allocator APIs — arena, pool, region allocators (Plan §6.7)

---

## 15. Developer Experience

- [x] Compiler warnings — unused variables (`W0001`), unreachable code (`W0002`)
      with stable IDs (Plan §17.2)
- [x] Suggested fixes — diagnostics carry a `help:` line for common errors (Plan §17.1)
- [x] Machine-readable diagnostic output — `a check --json` (Plan §16)

---

## 16. Memory Management

- [ ] Ownership model — move semantics, transfer rules (Plan §6.2)
- [ ] Borrowing rules — immutable vs mutable references, lifetimes (Plan §6.3)
- [ ] Deterministic cleanup — destructors / scoped guards (Plan §6.4)
- [ ] Unsafe operations — raw pointers, unchecked memory access (Plan §6.9)

---

## Legend

- [ ] ⬜ — Not implemented
- [x] ✅ — Implemented

---

## Recently Completed

- [x] Python-compatible `str`/`list` method surface (2026-10-29)
- [x] Free `int(x)` / `float(x)` conversions (2026-10-29)
- [x] Compiler warnings with stable codes: `W0001` unused variable, `W0002`
      unreachable statement (2026-10-29)
- [x] Machine-readable diagnostics: `a check --json` (2026-10-29)
- [x] `a test` runner for `fn test_*()` functions (2026-10-29)
- [x] Map/set literals rejected at compile time instead of failing at runtime
      with an internal error (2026-10-29)
- [x] TODO audit: confirmed implemented — raw strings, open-ended ranges,
      class inheritance, generics, type aliases, enum values, `a check/fmt/doc`
      (2026-10-29)
- [x] `++` / `--` prefix/postfix increment and decrement (2026-10-28)
- [x] All four loop types: `while`, `do...while`, `for ... in`, `loop`
- [x] `break` / `continue` loop control
- [x] `match` pattern matching
- [x] `|>` pipeline operator
- [x] `?.` / `??` / `!` null-safe operators
- [x] List comprehensions `[expr for var in iterable]`
- [x] Stepped ranges `0..10..2`
- [x] Negative indexing `items[-1]`
- [x] String interpolation `"{name}"`
- [x] `a-lsp` language server
- [x] Docs updated: `docs/language-spec/README.md` (keywords, control flow, `++`/`--`)
- [x] Docs updated: `docs/tutorials/README.md` (loops reference)
- [x] Example: `examples/loops/demo_inc_dec.a`
