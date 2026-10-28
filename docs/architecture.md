# Toolchain Architecture

The A toolchain is a layered C++20 codebase. Every compiler stage is its own
static library with a public header under `include/a/` and a matching
implementation under `src/`. All code lives in `namespace a`.

## Module graph

```
a_common      SourceFile, Span, Token, DiagnosticSink
   ^
a_frontend    Lexer, AST, Parser
   ^
a_sema        Type, TypePtr, Sema, SemaResult
   ^
a_runtime     Value, Interpreter
   ^
a_driver      DriverConfig, runFile, runSource
   ^
executable `a` (src/cli/main.cpp)
```

Dependencies point only upward. The driver depends on every stage; the CLI
depends only on the driver. CMake exposes namespaced aliases (`a::common`,
`a::frontend`, `a::sema`, `a::runtime`, `a::driver`) via `add_library(... ALIAS)`.

## Pipeline

`runSource` in `src/driver/driver.cpp` drives the stages and owns the exit
codes:

1. **Lex** — `Lexer` produces a token stream, reporting lexical errors.
2. **Parse** — `Parser` builds a `Program` AST, recovering at line boundaries.
3. **Analyze** — `Sema` resolves names, infers/unifies types, and type-checks.
4. **Execute** — `Interpreter` walks the AST (skipped by `a check`).

Diagnostics are accumulated in a single `DiagnosticSink` and flushed after each
phase, so a lexical error and a type error never interleave incorrectly. The
sink renders source lines, carets, and `help:` notes, with color enabled only
when stderr is a terminal.

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
invoking a sub-`Lexer`/`Parser` in inline mode for each `{...}` fragment. This
keeps the lexer single-pass and makes interpolation errors point at the nested
expression.

Raw strings (`"""..."""`) are scanned across newlines by the lexer itself. The
token carries `rawString = true`, line/column tracking continues through the
body, no `Newline`/`Indent` tokens are emitted for its interior, and the parser
treats the whole token as one literal part. One newline directly after the
opening quotes is stripped; nothing is dedented.

Multi-line pipelines rely on a lexer rule: when a line's first non-space
characters are `|>`, the preceding newline is suppressed and indentation is not
recomputed. The parser therefore sees one continuous expression.

## Parser

`Parser` is a recursive-descent parser for statements and a Pratt parser for
expressions. The precedence table lives in `binaryPrecedence`
(`|>` 1, `??` 2, `or` 3, `and` 4, prefix `not` 5, comparisons 6, `..` 7,
`+ -` 8, `* / %` 9, prefix `-` 10, right-associative `**` 11); slice bounds
parse at 8. Statements dispatch on the leading token, with a small special case
for typed declarations (`name: Type = value`) and destructuring (`(a, b) = ...`,
`var (a, b) = ...`).

Pipelines are desugared in the parser: after parsing the right side of `|>`,
it must be a `CallExpr` whose callee is a plain identifier, and the left value
is inserted as the first argument. Anything else is a parse diagnostic.

`match` parses as a block-shaped expression: subject, `:`, newline, indented
arms. Arms are `pattern [if guard] => expression`, and the closing dedent ends
the expression. `expectNewline()` accepts a dedent or a just-closed block so
statements whose expression ends with a block (`x = match ...`) terminate
correctly. `(` starts a grouped expression or a tuple depending on the comma;
`[` starts an array literal or a comprehension when `for` follows the first
element.

Error recovery uses a `ParseError` exception caught at statement boundaries;
`synchronize()` skips to the next newline. The parser never throws out of
`parseProgram`, so every source file yields an AST (possibly partial) and a
list of diagnostics.

## Semantic analysis

`Sema` performs five logical passes over the program:

1. register top-level function and struct names;
2. resolve struct layouts and method signatures;
3. resolve function signatures;
4. check top-level statements (which declares globals);
5. check function and method bodies.

Function bodies see a copy of the global scope, so functions can reference
globals declared later in the file while still detecting duplicate locals.
Type inference uses constrained type variables:

- literals create variables constrained to `Integer`/`Floating`/`Numeric`;
- unification links variables, merging constraints, defaultability, and the
  `allowUnresolved` flag (used by the element type of a bare `None`);
- unresolved, defaultable variables become `i32` or `f64` at the end;
- anything still unresolved is reported as an inference error unless it
  arrived from `None`.

Structured types are unified recursively: `Optional` through `elem`, `Tuple`
and `Function` through `params` (plus `ret` for functions). Higher-order
builtins (`map`, `filter`, `reduce`, `any`, `all`, `sum`, `join`) check lambda
arguments against an expected `Function` type, so untyped lambda parameters are
inferred from context. Method dispatch on structs prefers a real method and
falls back to a function-typed field; the runtime follows the same order.

`SemaResult` hands the interpreter the function and struct lookup tables that
were successfully analyzed.

## Runtime

`Value` is a `std::variant` of primitives, `String`, array pointers, struct
pointers, ranges, optionals (`OptionalValue`), tuples (`TupleValue`), and
closures (`Closure`). Arrays, structs, optionals, and tuples are shared by
reference, matching the documented semantics.

`Interpreter` uses lexical environments (`Env` chains) and exceptions for
non-local control flow (`ReturnSignal`, `BreakSignal`, `ContinueSignal`). A
runtime error is captured as a `RuntimeError` with a span and help text,
reported through the shared `DiagnosticSink`, and mapped to exit code 2.

Closures are captured by value: when a lambda is created, visible local
bindings are copied into the `Closure` (globals are excluded and resolved at
call time through `globalEnv_`). A `Closure` either points at a `LambdaExpr`
body or at a named `FnDecl` (functions used as values), and calls reuse the
call-depth guard. `match` evaluates arms in order using structural
`valuesEqual`, with inclusive range patterns; comprehensions iterate a range,
an array snapshot, or a string's code points. String indexing and slicing walk
UTF-8 with `decodeUtf8`, while byte-oriented methods keep using byte search.
Stepped slices require a positive step and reject anything else at runtime.

Two details worth preserving when extending the runtime:

- `execStatements` collects `defer` bodies and runs them in reverse order even
  when a signal or error is unwinding the block, rethrowing the pending
  exception afterwards.
- Integer operations use overflow-checked builtins; division, modulo, and
  conversions check their domains explicitly.

## Adding a feature

A typical language addition touches each layer in order:

1. token(s) in `include/a/common/token.hpp` and `src/frontend/lexer.cpp`;
2. AST node in `include/a/frontend/ast.hpp`, parsing in `parser.cpp`;
3. typing rules in `src/sema/sema.cpp` (and helpers in `types.hpp`);
4. evaluation in `src/runtime/interpreter.cpp`;
5. golden tests in `tests/cases/` and a reference entry in
   `docs/language.md`.

Keeping those steps separated is intentional: the modules should remain
independently testable and reusable by future front ends (formatter, language
server) and back ends (LLVM, WASM).
