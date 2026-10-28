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

The current release (v0.2.0) ships the complete front end and a tree-walking
interpreter:

- indentation-aware lexer with string interpolation and raw strings
- recursive-descent parser with error recovery
- multi-pass semantic analysis with unification-based type inference
- interpreter with checked arithmetic, `defer`, structs, methods, lambdas,
  optionals, tuples, `match`, comprehensions, and pipelines
- string/array/math builtins plus file I/O

`a run` executes programs directly; `a check` type-checks without running.
Native code generation (LLVM) is the next milestone and is not part of v0.2.

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
tests/run_tests.sh build/a
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

See [docs/language.md](docs/language.md) for the full reference and
[docs/architecture.md](docs/architecture.md) for the compiler internals.
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
- **Optionals without noise.** `T?`, `None`, `Some(x)`, safe navigation
  (`user?.name`), lazy fallbacks (`x ?? 0`), and postfix unwrap (`x!`).
- **UTF-8 strings.** Strings are immutable UTF-8 sequences, indexed and sliced
  by Unicode code point.

## Project layout

```
include/a/common    source text, spans, tokens, diagnostics
include/a/frontend  lexer, parser, AST
include/a/sema      type system and semantic analysis
include/a/runtime   values and the interpreter
include/a/driver    compiler pipeline entry points
src/...             implementations mirroring the include tree
src/cli/main.cpp    command-line interface
tests/              golden-file suite
examples/           sample programs
docs/               language and architecture references
```

Each compiler stage is a separate static library (`a_common`, `a_frontend`,
`a_sema`, `a_runtime`, `a_driver`) with namespaced CMake aliases (`a::sema`,
`a::runtime`, ...) so that the stages can be embedded or reused independently.

## Limitations (v0.2)

- No native code generation yet; programs run in the interpreter.
- No module system (`import` is parsed but rejected with a clear diagnostic).
- No generics, enums, interfaces, or `Result` types.
- Lambdas capture locals by value at creation; later mutation is not observed.
- Tuples cannot be indexed or compared with `==`.
- Integers are stored as signed 64-bit values, so `u64` covers only the
  non-negative half of its range and there is no `U64_MAX` constant.
- Arrays are reference values; slicing creates a copy.

The full roadmap lives in [Plan.md](Plan.md).
