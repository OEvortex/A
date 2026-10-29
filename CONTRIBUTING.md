# Contributing to A

Thanks for helping improve the A language toolchain.

## Building

```sh
tooling/build/build.sh          # Release build with warnings as errors + tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
```

## Testing

```sh
ctest --test-dir build --output-on-failure
tests/integration/run_tests.sh build/a
python3 tools/language-server/smoke_test.py build/a-lsp
for f in examples/*/*.a; do build/a run "$f" >/dev/null || echo "FAIL $f"; done
```

The same suite runs on GitHub Actions (`.github/workflows/ci.yml`) on every
push and pull request, including the version-consistency check and an
ASan/UBSan job.

Golden tests live in `tests/integration/cases`. Each `<name>.a` may have
companion files: `<name>.out` (stdout), `<name>.in` (stdin), `<name>.err`
(stderr substrings), `<name>.exit` (exit code), and `<name>.flags` (`check`
runs `a check` instead of `a run`).

## Layout

```
A/
│
├── ac                          # Instant runner shortcut (`./ac prog.a`)
├── CMakeLists.txt              # One build: a, a-lsp, a-docgen, a-tooling-tests
├── README.md  CONTRIBUTING.md  SECURITY.md  CODE_OF_CONDUCT.md  LICENSE
├── .editorconfig               # 4 spaces for A/Markdown, 2 for JSON/YAML/C++
│
├── .github/
│   ├── workflows/ci.yml        # Build + ctest, version-consistency, sanitizers
│   ├── ISSUE_TEMPLATE/
│   └── PULL_REQUEST_TEMPLATE.md
│
├── compiler/source/
│   ├── lexer/  parser/  ast/   diagnostics/
│   ├── name-resolution/        # Module loader, native module table, builtin docs
│   ├── type-checker/           # Sema, types, method signature tables
│   ├── semantic-analysis/  ownership/
│   ├── ir/  optimization/
│   ├── backend/                # Interpreter, native (C++) and LLVM codegen
│   ├── tooling/                # AST formatter, doc generator, IR printers
│   ├── driver/  cli/           # Version, linking glue, `a` subcommands
│   └── tests/
│
├── runtime/                    # Values, memory, UTF-8 strings, collections, IO
│
├── stdlib/                     # Module docs/notes and A-file modules:
│   ├── core/  math/  collections/  fs/  time/  test/
│   └── json/  binary/  testing/  log/  metrics/  trace/  net/  http/
│
├── tools/
│   ├── language-server/        # a-lsp (stdio LSP) + smoke_test.py
│   ├── vscode-a/               # VS Code extension: grammar + dependency-free client
│   ├── formatter/  documentation/  benchmark-runner/
│
├── tests/
│   ├── integration/            # Golden cases, run_tests.sh
│   ├── lexer/  parser/  type-system/  runtime/  memory-safety/  ir/  tooling/
│
├── benchmarks/                 # Workloads with cross-language equivalents
├── docs/                       # Language spec, type system, memory model,
│                               # compiler design, standard library, performance
├── examples/                   # Runnable examples, indexed by examples/README.md
└── tooling/                    # build.sh, release.sh helper scripts
```

## Conventions

- **Commits**: Conventional-style subjects scoped by area, e.g.
  `feat(lsp): complete native module members`, `fix(sema): report arity on
  pipe calls`. Imperative, one logical change per commit.
- **Pull requests**: one behavior per PR, description covers *why*; link
  issues. The PR template lists the checks — formatter/doc-generator or LSP
  behaviour changes should extend the golden cases or `smoke_test.py`.
- **Versioning**: `CMakeLists.txt`, `compiler/source/driver/version.hpp`,
  `tools/language-server/server.cpp` (`kServerVersion`) and
  `tools/vscode-a/package.json` must agree; CI fails if they drift.
- **Documentation**: new stdlib members need a row in
  `docs/standard-library/README.md`, an entry in `builtin_docs.hpp` (so the
  LSP can hover/complete them), and a runnable example when they are not
  covered by an existing one.

## Style

- C++20, `namespace a`, headers next to their implementations.
- Keep warnings clean; CI builds with `-Werror`.
- Prefer clarity over cleverness in the compiler; the interpreter favors
  readable tree-walking code over premature optimization.
