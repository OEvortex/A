# A

A is an indentation-based, statically typed programming language with type
inference and a compact, predictable surface. This repository contains the
reference toolchain, written in C++20.

```a
struct Point:
    x: i32
    y: i32

    fn length_squared(self) -> i32:
        return self.x * self.x + self.y * self.y

point = Point(3, 4)
print("length^2 = {point.length_squared()}")
```

## Status

The current release (v0.3.0) ships the complete front end and a tree-walking
interpreter:

- indentation-aware lexer with string interpolation and raw strings
- recursive-descent parser with error recovery
- multi-pass semantic analysis with unification-based type inference
- multi-file programs with `import`, `import ... as`, and `from ... import`,
  Python-style run-once module semantics, and native modules
- interpreter with checked arithmetic, `defer`, structs, methods, enums,
  lambdas, optionals, tuples, `match`, comprehensions, and pipelines
- C-level operators: bitwise, shifts, compound assignment, and `?:`
- `const` bindings, `do ... while`, stepped ranges, and Unicode escapes
- string/array/math builtins, file I/O, filesystem, time, and assertion helpers
- an A-file standard library under `stdlib/`

`a run` executes programs directly; `a check` type-checks without running.
Native code generation (LLVM) is the next milestone and is not part of v0.3.

### Performance

The v0.3 interpreter is 10×–330× slower than optimized C++ on compute-only
measurements (2.3×–7.6× end-to-end) and is now faster than CPython on every
reference suite. The first optimization phase — tagged AST dispatch, enum
operators, flat inline scope maps, loop-scope reuse, and cached call-site
resolution — is implemented; the measured before/after and the phased plan to
native parity (slot frames, bytecode VM, typed IR, LLVM backend) are in
[`docs/performance`](docs/performance/README.md). Compare all three languages:

```sh
python3 tools/benchmark-runner/compare.py --runs 5
```

## Build

Requirements: CMake 3.20+, a C++20 compiler (GCC 11+, Clang 14+, MSVC 19.29+).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The binary is `build/a`. Run the test suite with:

```sh
ctest --test-dir build --output-on-failure
# or directly:
tests/integration/run_tests.sh build/a
```

## Usage

```sh
a run program.a       # compile and execute
a check program.a     # type-check only
a version             # print the toolchain version
a help                # show usage
```

Options: `--no-color` disables colored diagnostics (they are also disabled
automatically when stderr is not a terminal).

Exit codes:

| Code | Meaning |
| ---- | ------- |
| 0    | success |
| 1    | compile error (lex, parse, or type error) |
| 2    | runtime error |
| 64   | invalid command-line usage |
| 66   | input file could not be read |

## Language tour

See [docs/language-spec](docs/language-spec/README.md) for the full reference and
[docs/compiler-design](docs/compiler-design/README.md) for the compiler internals.
Examples live in [`examples/`](examples).

Highlights:

- **Immutable by default.** `x = 1` declares an immutable binding; `var x = 1`
  allows rebinding. Struct fields and array elements remain mutable.
- **Inference everywhere.** Literals, parameters, and return types can all be
  inferred; bindings may still carry explicit annotations (`x: i32 = 1`).
- **Strict numerics.** Arithmetic requires matching types. Use the conversion
  functions (`i64`, `f64`, ...) for explicit, range-checked conversions.
- **Checked arithmetic.** Integer overflow, division by zero, and out-of-range
  conversions are runtime errors with source locations instead of silent
  wraparound.
- **Deterministic cleanup.** `defer` blocks run in reverse order when the
  enclosing block exits, including through `return` and errors.
- **Ergonomic syntax.** Pipelines (`|>`), comprehensions, `match` expressions,
  lambdas, tuples with destructuring, and stepped slices.
- **Modules.** `import "lib/x.a" as x`, `from "lib/x.a" import fn`, and native
  `import math`. Modules run once in dependency order; exported `var`s are
  live bindings.
- **Optionals without noise.** `T?`, `None`, `Some(x)`, safe navigation
  (`user?.name`), lazy fallbacks (`x ?? 0`), and postfix unwrap (`x!`).
- **UTF-8 strings.** Strings are immutable UTF-8 sequences, indexed and sliced
  by Unicode code point.
- **C-level breadth, simple surface.** Bitwise/shift operators, `?:`, C-style
  enums, `const`, and `do ... while` — without pointers, manual memory, or
  macros.

## Project layout

```
compiler/source/lexer          source text, spans, tokens, lexer
compiler/source/ast            AST definitions
compiler/source/parser         recursive-descent parser
compiler/source/diagnostics    spans-aware diagnostic rendering
compiler/source/name-resolution  module loading and resolution
compiler/source/type-checker   type system and semantic analysis
compiler/source/backend        the interpreter
compiler/source/driver         compiler pipeline entry points
compiler/source/cli            command-line interface
runtime/memory                 value representation
runtime/strings                UTF-8 and formatting helpers
runtime/errors                 runtime error types
runtime/collections            transparent string-keyed tables
stdlib/                        native and A-file library modules
tests/integration              golden-file suite
tools/                         formatter, LSP, docs, benchmark runner
benchmarks/                    performance suite and cross-language equivalents
examples/                      sample programs by topic
tooling/                       build, release, and CI scripts
docs/                          language, design, library, and performance references
```

Each compiler stage is a separate static library (`a_diagnostics`, `a_lexer`,
`a_parser`, `a_name_resolution`, `a_type_checker`, `a_runtime`, `a_backend`,
`a_driver`) with namespaced CMake aliases (`a::type_checker`, `a::backend`,
...) so that the stages can be embedded or reused independently.

## Limitations (v0.3)

- No native code generation yet; programs run in the interpreter.
- No generics, interfaces, or `Result` types.
- Lambdas capture locals by value at creation; later mutation is not observed
  (module globals are read live).
- Tuples cannot be indexed or compared with `==`.
- Integers are stored as signed 64-bit values, so `u64` covers only the
  non-negative half of its range and there is no `U64_MAX` constant.
- Arrays are reference values; slicing creates a copy.

The full roadmap lives in [Plan.md](Plan.md).
