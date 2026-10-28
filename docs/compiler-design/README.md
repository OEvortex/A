# Toolchain Architecture

The A toolchain is a layered C++20 codebase under `compiler/` and `runtime/`.
Headers live next to their sources, and every compiler stage is its own static
library. All code lives in `namespace a`.

## Layout

```
compiler/source/
  lexer/            token.hpp source.hpp span.hpp lexer.{hpp,cpp}
  ast/              ast.hpp
  parser/           parser.{hpp,cpp}
  diagnostics/      diagnostics.{hpp,cpp}
  name-resolution/  module_loader.{hpp,cpp} native_modules.hpp
  type-checker/     types.hpp sema.{hpp,cpp}
  backend/          interpreter.{hpp,cpp}
  driver/           driver.{hpp,cpp} version.hpp
  cli/              main.cpp
runtime/
  memory/value.hpp
  strings/          utf8.{hpp,cpp} format.{hpp,cpp}
  errors/           runtime_error.hpp
  collections/      transparent_map.hpp
```

## Library graph

```
a_diagnostics       FileRegistry, DiagnosticSink
   ^
a_lexer             SourceFile, Span, Token, Lexer
   ^
a_parser            AST, Parser
   ^
a_name_resolution   ModuleLoader, ModuleGraph, native module specs
   ^
a_type_checker      Type, TypePtr, Sema, SemaResult
   ^
a_backend           Value, Interpreter
   ^
a_driver            DriverConfig, runFile, runSource
   ^
executable `a` (compiler/source/cli/main.cpp)
```

`a_runtime` (UTF-8 and value formatting) sits alongside the front end and is
linked by `a_backend`. Dependencies point only upward; CMake also exposes
`a::diagnostics`, `a::lexer`, `a::parser`, `a::name_resolution`,
`a::type_checker`, `a::runtime`, `a::backend`, and `a::driver` aliases.

Release builds enable link-time optimization with `A_ENABLE_LTO` (default on);
the driver and runtime use transparent string-keyed tables
(`runtime/collections/transparent_map.hpp`) so name lookups can search with
`std::string_view` without allocating.

## Pipeline

`runSource` in `compiler/source/driver/driver.cpp` owns the exit codes:

1. **Load** — `ModuleLoader` parses the entry file and every import, resolving
   paths, detecting cycles, and producing a post-order `ModuleGraph`.
2. **Analyze** — `Sema` runs per module in dependency order, inferring and
   unifying types and recording module export tables.
3. **Execute** — `Interpreter` runs each module's top-level statements once, in
   dependency order, ending with the entry module (skipped by `a check`).

Tokens carry a `fileId`; `DiagnosticSink` holds a registry of `SourceFile`s and
renders the diagnostic against the owning file. `SourceManager` keeps the files
alive for the whole pipeline.

## Lexer

`Lexer` is indentation-aware. It maintains:

- an `indentStack` of column widths used to synthesize `Indent`/`Dedent`
  tokens at line starts;
- a bracket depth counter so newlines inside `(...)`, `[...]`, and `{...}` do
  not emit `Newline` or indentation tokens;
- an inline mode used for string interpolations, where physical layout is not
  significant.

String interpolation is not expanded in the lexer. The parser sees the raw
contents of a string token and splits it into literal and expression parts,
invoking a sub-`Lexer`/`Parser` in inline mode for each `{...}` fragment.
`\u{...}` escapes are copied verbatim before brace scanning so they are not
mistaken for interpolation.

Raw strings (`"""..."""`) are scanned across newlines by the lexer itself; the
token carries `rawString = true`, line/column tracking continues through the
body, and the parser treats the whole token as one literal part. One newline
directly after the opening quotes is stripped; nothing is dedented.

Multi-line pipelines rely on a lexer rule: when a line's first non-space
characters are `|>`, the preceding newline is suppressed and indentation is not
recomputed, so the parser sees one continuous expression.

## Parser

`Parser` is a recursive-descent parser for statements and a Pratt parser for
expressions. The precedence table lives in `binaryPrecedence` in
`compiler/source/parser/parser.cpp`:

`|>` 1, `?:` 2 (right), `??` 3, `or` 4, `and` 5, prefix `not` 6,
comparisons 7, `..` 8, `|` 9, `^` 10, `&` 11, shifts 12, `+ -` 13,
`* / %` 14, prefix `- ~` 15, right-associative `**` 16. Slice bounds parse at
13.

Statements dispatch on the leading token, including `const`, `enum`, `do`, and
the import forms. Typed declarations (`name: Type = value`) and destructuring
(`(a, b) = ...`, `var (a, b) = ...`) are handled as special cases. `enum`
parses a name and an indented variant list; `do` parses a suite followed by
`while condition`; ranges become `RangeExpr` nodes with an optional step.

Pipelines are desugared in the parser: after parsing the right side of `|>`,
it must be a `CallExpr` whose callee is a plain identifier, and the left value
is inserted as the first argument.

`match` parses as a block-shaped expression: subject, `:`, newline, indented
arms. Arms are `pattern [if guard] => expression`, and the closing dedent ends
the expression. `(` starts a grouped expression or a tuple depending on the
comma; `[` starts an array literal or a comprehension when `for` follows the
first element.

Error recovery uses a `ParseError` exception caught at statement boundaries;
`synchronize()` skips to the next newline, so every source file yields an AST
(possibly partial) plus diagnostics.

## Module loading

`ModuleLoader` resolves imports as it descends:

- quoted paths resolve against the importing file's directory, then each `-I`
  directory, then `A_PATH`, then the stdlib root;
- bare names resolve to native modules first, then `stdlib/<name>.a` or
  `stdlib/<name>/<name>.a`;
- paths are canonicalized with `weakly_canonical`, loads are cached by
  canonical path, and a visiting-state DFS reports import cycles at the
  offending span;
- `resolvedModule` is stamped onto each import node, and `graph.order` is the
  post-order used for analysis and execution.

## Semantic analysis

`Sema` analyzes each module in dependency order. For every module it performs:

1. bind imports (module aliases and `from ... import` symbols);
2. register top-level struct, enum, and function names;
3. resolve struct layouts and method signatures;
4. resolve function signatures;
5. check top-level statements (declaring globals);
6. check function and method bodies.

Tables are keyed by qualified names (`module::name`); `currentModule_` is saved
and restored around body checks. Function bodies see a copy of the module's
global scope, so they can reference globals declared later in the same file
while still detecting duplicate locals.

Type inference uses constrained type variables: literals create
`Integer`/`Floating`/`Numeric` variables, unification merges constraints and
defaultability, unresolved defaultable variables become `i32`/`f64`, and
anything else is an inference error. Enums are `i32`-backed, so enum-typed
bindings unify with their variant values; the enum namespace itself has type
`Enum` for member access. Module values have type `Module`, which `Sema` uses
to resolve `m.fn`, `m.Struct(...)`, `m.Color.Variant`, and live `m.value`.

`SemaResult` hands the interpreter the per-module export tables, qualified
function/struct/enum tables, and the module id of every declaration.

## Runtime

`Value` is a `std::variant` of primitives, `String`, array pointers, struct
pointers, ranges (start, end, step), optionals, tuples, closures, modules, and
native functions. Arrays, structs, optionals, and tuples are shared by
reference.

`Interpreter` keeps one `Env` per module in a `std::deque` (stable addresses).
Each `callFunction` frame chains to its module globals; lambdas capture locals
by value and stop at their defining module env, while globals resolve live at
call time. Exceptions implement non-local control flow (`ReturnSignal`,
`BreakSignal`, `ContinueSignal`). Runtime errors carry a span and help text,
are reported through the shared `DiagnosticSink`, and exit with code 2.

Modules execute once: `run()` iterates `ModuleGraph::order`, and import
statements only bind already-created module values. `m.name` reads the live
binding and assignment through `m.name = v` writes it. Native modules are thin
namespaces over builtins, dispatched through `NativeFunction` values.

Two details worth preserving when extending the runtime:

- `execStatements` collects `defer` bodies and runs them in reverse order even
  when a signal or error is unwinding the block, rethrowing afterwards.
- Integer operations are overflow-checked; shifts validate 0..63, `<<` wraps
  on 64 bits, and division/modulo/conversions check their domains.

## Performance

The v0.3 backend is a tree-walking interpreter, and every optimization decision
for the future VM/LLVM backends must keep its semantics (checked arithmetic,
`defer` unwinding, reference-valued arrays). The measured baseline, per-phase
targets, and the C++-parity gates are in
[`docs/performance`](../performance/README.md).

Known hot-path costs, in order of impact:

- AST walking: one `switch` per node on a kind tag, plus a call and span copy;
  the compiler-shaped alternative is a bytecode/register VM (P4);
- 40-byte `std::variant` `Value` construction and copies (`P2` shrinks and
  unboxes this);
- string-compare scope lookup and parent-chain walks (`Env::find`); P3 replaces
  this with a `(depth, slot)` index resolved by `Sema`;
- argument vectors allocated per call; unboxed parameters follow from P2/P3;
- no cross-node constant folding or specialization despite `Sema` knowing every
  type.

Already removed in the P1 pass: RTTI `dynamic_cast` dispatch, string comparison
for operator dispatch, per-iteration scope/hash-table allocation in `for` and
comprehensions, and repeated call-site resolution.

## Adding a feature

A typical language addition touches each layer in order:

1. token(s) in `compiler/source/lexer/token.hpp` and `lexer.cpp`;
2. AST node in `compiler/source/ast/ast.hpp`, parsing in `parser.cpp`;
3. typing rules in `type-checker/sema.cpp` (and helpers in `types.hpp`);
4. evaluation in `backend/interpreter.cpp`;
5. golden tests under `tests/integration/cases/` and a reference entry in
   `docs/language-spec/README.md`.

Keeping those steps separated is intentional: the modules remain independently
testable and reusable by future front ends (formatter, language server) and
back ends (LLVM, WASM).
