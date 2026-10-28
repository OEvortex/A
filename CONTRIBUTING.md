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
for f in examples/*/*.a; do build/a run "$f" >/dev/null || echo "FAIL $f"; done
```

Golden tests live in `tests/integration/cases`. Each `<name>.a` may have
companion files: `<name>.out` (stdout), `<name>.in` (stdin), `<name>.err`
(stderr substrings), `<name>.exit` (exit code), and `<name>.flags` (`check`
runs `a check` instead of `a run`).

## Layout

a-language/
│
├── README.md
├── LICENSE
├── CONTRIBUTING.md
├── SECURITY.md
│
├── docs/
│   ├── language-spec/
│   ├── compiler-design/
│   ├── memory-model/
│   ├── type-system/
│   ├── standard-library/
│   └── tutorials/
│
├── compiler/
│   ├── source/
│   │   ├── lexer/
│   │   ├── parser/
│   │   ├── ast/
│   │   ├── diagnostics/
│   │   ├── name-resolution/
│   │   ├── type-checker/
│   │   ├── semantic-analysis/
│   │   ├── ownership/
│   │   ├── ir/
│   │   ├── optimization/
│   │   ├── backend/
│   │   └── driver/
│   │
│   └── tests/
│
├── runtime/
│   ├── memory/
│   ├── strings/
│   ├── collections/
│   ├── io/
│   ├── errors/
│   └── concurrency/
│
├── stdlib/
│   ├── core/
│   ├── math/
│   ├── collections/
│   ├── fs/
│   ├── net/
│   ├── json/
│   ├── time/
│   └── test/
│
├── tools/
│   ├── formatter/
│   ├── language-server/
│   ├── documentation/
│   └── benchmark-runner/
│
├── tests/
│   ├── lexer/
│   ├── parser/
│   ├── type-system/
│   ├── runtime/
│   ├── memory-safety/
│   └── integration/
│
├── benchmarks/
│   ├── arithmetic/
│   ├── collections/
│   ├── memory/
│   ├── numerical/
│   └── real-world/
│
├── examples/
│   ├── hello-world/
│   ├── input-output/
│   ├── loops/
│   ├── functions/
│   ├── collections/
│   └── structs/
│
└── tooling/
    ├── build/
    ├── release/
    └── ci/

## Style

- C++20, `namespace a`, headers next to their implementations.
- Keep warnings clean; CI builds with `-Werror`.
- Prefer clarity over cleverness in the compiler; the interpreter favors
  readable tree-walking code over premature optimization.
