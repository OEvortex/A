<p align="center">
  <h1 align="center"><b>The A Programming Language</b></h1>
  <p align="center">
    <strong>Fast as C++. Expressive as Python. Statically typed. Memory safe.</strong>
  </p>
  <p align="center">
    <a href="#-benchmarks">Benchmarks</a> •
    <a href="#-quickstart">Quickstart</a> •
    <a href="#-language-tour">Language Tour</a> •
    <a href="#-tooling">Tooling</a> •
    <a href="#-editor-support">Editor Support</a> •
    <a href="#-key-features">Features</a> •
    <a href="docs/">Docs</a>
  </p>
</p>

---

## ⚡ What is A?

**A** is a high-performance, statically typed systems and application language designed with modern indentation-based syntax. It brings together the speed of native machine code and the ergonomic elegance of Python, giving developers predictable execution, zero-overhead abstractions, and unmatched readability.

```a
class Vector2D:
    x: float
    y: float

    fn length_squared(self) -> float:
        return self.x ** 2.0 + self.y ** 2.0

point = Vector2D(3.0, 4.0)
print("Length squared: {point.length_squared()}")  # 25.0
```

---

## 🚀 Key Features

* **⚡ C++ Parity Performance**: Native compilation (`a build`) with sub-4 ms startup, matching optimized C++ (`g++ -O2`) on arithmetic, pipeline traversals, and string allocation.
* **🐍 Python-Grade Ergonomics**: Python-style natural variable declarations (`x = 10`), clean indented blocks, and the instant `ac` runner.
* **🏛️ Classes, Inheritance & Metaclasses**: Clean object-oriented programming with `class Child(Parent):` and declarative `metaclass Meta: pass`.
* **🛠️ 25+ First-Class Operators**: Rich operator support including arithmetic (`+ - * / % **`), bitwise (`& | ^ ~ << >>`), containment (`in`, `not in`), functional pipelines (`|>`), and safe null-coalescing (`??`, `?.`).
* **🔒 Static Type Safety & Inference**: Unification-based global type inference with checked arithmetic, zero undefined behavior, and deterministic cleanup via `defer`.
* **🧰 Production Tooling**: An AST formatter (`a fmt`), documentation generator (`a doc`), dependency-free language server (`a-lsp`), and VS Code extension with live diagnostics, navigation, completion, formatting, rename, folding, and syntax highlighting.

---

## 📊 Benchmarks

Measured over 7 runs against **C++ (`g++ -O2`)** and **CPython 3.13** on identical workloads:

| Workload | Task | A Native (`a build`) | C++ (`-O2`) | Python 3.13 | Parity vs. C++ |
|:---|:---|:---:|:---:|:---:|:---:|
| **Arithmetic** | 200,000 range additions | **3.23 ms** | 3.46 ms | 56.05 ms | **1.0× (Native Parity)** |
| **Collections** | 5,000-element pipeline | **3.25 ms** | 3.46 ms | 33.39 ms | **1.0× (Native Parity)** |
| **Memory** | 100k `str()` + join | **7.25 ms** | 6.70 ms | 47.91 ms | **1.1× (Native Parity)** |
| **Numerical** | 50,000 `sqrt` + `round` | **4.00 ms** | 3.91 ms | 44.85 ms | **1.0× (Native Parity)** |
| **Process Startup** | CLI launch latency | **3.96 ms** | 3.29 ms | 31.36 ms | **~1.1× (Sub-4 ms)** |

*Run benchmarks locally with `python3 tools/benchmark-runner/compare.py --native --runs 5`.*

---

## 🛠 Quickstart

### Prerequisites
* CMake 3.20+
* C++20 compiler (GCC 11+, Clang 14+, or MSVC 19.29+)
* `g++` on `PATH` for `a build` — the native backend emits C++ and shells out
  to `g++ -O3 -std=c++20`

### Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Running Programs
```bash
# 1. Instant execution (shortcut):
./ac examples/classes/classes.a

# 2. Standard execution:
./build/a run program.a

# 3. Type-check without running:
./build/a check program.a

# 4. Run every `fn test_*()` in a program:
./build/a test program.a

# 5. Compile to an optimized standalone native binary:
./build/a build program.a -o my_app
./my_app

# 6. Format and document it:
./build/a fmt -w program.a
./build/a doc -o docs/program.md program.a

# 7. Inspect the typed SSA IR (optionally after optimization):
./build/a ir program.a
./build/a ir --opt program.a
```

---

## 🔍 Language Tour

### 1. Variables & Clean Declarations
```a
# Natural variable assignment (mutable in place)
count = 10
count += 5
print(count)  # 15

# Explicit immutable constants
const PI = 3.14159265359
const APP_NAME: str = "A-Engine"
```

### 2. Classes & Inheritance
```a
class Shape:
    name: str

    fn describe(self) -> str:
        return "Shape: {self.name}"

class Circle(Shape):
    radius: float

    fn area(self) -> float:
        return 3.14159 * (self.radius ** 2.0)

c = Circle("UnitCircle", 10.0)
print(c.describe(), "Area:", c.area())
```

### 3. Functional Pipelines & Comprehensions
```a
# Clean readable data pipelines using |>
sum_of_evens = [x for x in 0..100]
    |> filter(fn(n) => n % 2 == 0)
    |> map(fn(n) => n * 2)
    |> sum()

print("Result:", sum_of_evens)
```

### 4. Containment & Modern Operators
```a
items = ["apple", "banana", "cherry"]
if "banana" in items:
    print("Found banana!")

if "grape" not in items:
    print("Grape is missing")

# Safe null navigation & Elvis fallback
user_theme = user?.settings?.theme ?? "dark"
```

---

## 📚 Standard Library

`import math`, `from json import parse` — the toolchain resolves modules
against the file's directory, `-I` paths, `A_PATH`, and the stdlib root
(`--stdlib <dir>` to override). Native modules are built into the toolchain;
`stdlib/` additionally holds A-file modules and per-module notes.

| Module | Provides |
| :--- | :--- |
| `math` | `sqrt` `sin` `log` `round` `random` … plus `PI`, `E` |
| `io` | `print` `eprint` `input` `flush` |
| `fs` | read/write/append files, directory listing, rename |
| `strings` | `len` `str` `join` (plus Python-style dot methods on `String`) |
| `collections` | `map` `filter` `reduce` `any` `all` `sum` `set` (dot methods on arrays/maps/sets) |
| `core` | `panic` `assert` `assert_eq` |
| `time` | `now` `sleep` |
| `unsafe` `arena` | raw pointer access; bump/pool/region allocators |
| `json` `binary` | parsing, reading, pretty-printing; big-endian byte encode/decode |
| `testing` | seeded property testing (`for_all_int`, …) |
| `log` `metrics` `trace` | structured logging, Prometheus counters/gauges/histograms, span propagation |
| `net` `http` | TCP/UDP sockets, optional TLS, `get`/`post`/`serve` |
| `test` (A-file) | assertion harness: `check` `equal` `require` `report` |

Interpreter-backed modules (json, binary, testing, log, metrics, trace, net,
http) run under `a run` and `a test`; `a build` reports when a compiled backend
cannot lower them. The full member tables and semantics live in
[`docs/standard-library/`](docs/standard-library/README.md), and
[`examples/README.md`](examples/README.md) indexes runnable programs.

---

## 📂 Project Organization

```
├── ac                          # Instant CLI runner script
├── compiler/
│   ├── source/
│   │   ├── lexer/              # Indentation-aware lexer & tokenization
│   │   ├── parser/             # Recursive-descent parser
│   │   ├── ast/                # Typed AST nodes
│   │   ├── type-checker/       # Global Sema & unification type inference
│   │   ├── ownership/          # Move/borrow analysis (`move(x)`, view borrows)
│   │   ├── ir/                 # Typed SSA IR with basic blocks and phis
│   │   ├── optimization/       # Constant folding, DCE, inlining, LICM
│   │   ├── backend/
│   │   │   ├── interpreter.cpp # High-speed tagged development interpreter
│   │   │   └── native/         # Native AOT codegen hitting C++ parity
│   │   ├── tooling/            # AST formatter, doc generator, shared printers
│   │   └── cli/                # `a run|build|check|test|fmt|doc|ir`
├── runtime/                    # Values, memory models, UTF-8 strings, and file I/O
├── stdlib/                     # Module notes + A-file modules (math, json, net, …)
├── benchmarks/                 # Verification suites with cross-language equivalents
├── examples/                   # Runnable programs, indexed by examples/README.md
├── docs/                       # Language spec, type system, compiler design, performance
├── tools/
│   ├── formatter/              # a fmt documentation
│   ├── documentation/          # a doc documentation
│   ├── language-server/        # a-lsp: diagnostics, navigation, completion, hover
│   └── vscode-a/               # VS Code extension (grammar + LSP client)
└── tests/                      # Automated golden test suite
```

---

## 🧰 Tooling

The toolchain ships with first-party developer tools built on the same lexer,
parser, and AST as the compiler.

### Command line: `a`

```
a <command> [options] <file.a>

  run <file.a>     compile and execute a program
  build <file.a>   compile to a native binary (-o <bin>)
  check <file.a>   type-check without running (--json for structured output)
  test <file.a>    run every `fn test_*()` in a program
  fmt <file.a>     format a source file (prints to stdout)
  doc <file.a>     generate documentation (markdown, prints to stdout)
  ir <file.a>      print the typed SSA intermediate representation
  version | help

Options:
  -o <file>        output path (for 'build' and 'doc')
  -w, --write      rewrite the source file in place (for 'fmt')
  --check          exit nonzero if the file needs formatting (for 'fmt')
  --indent <n>     indentation width in spaces (for 'fmt', default 4)
  --json           emit structured JSON (for 'doc' and 'check')
  --opt            run optimization passes when printing IR (for 'ir')
  --backend <name> 'native' (default, emits C++ and invokes g++) or 'llvm'
  --emit-llvm      emit textual LLVM IR (.ll) instead of an executable
  -I <dir>         add a module search directory (repeatable)
  --stdlib <dir>   override the standard library directory
  --no-color       disable colored diagnostics
```

### Formatter: `a fmt`

An AST-based formatter that normalises indentation, spacing, declarations, and
expressions while preserving comments, literal spelling, and semantics.

```bash
a fmt program.a             # print formatted source
a fmt --check program.a     # CI check: exit 1 when changes are needed
a fmt -w program.a          # rewrite in place
a fmt -w --indent 2 app.a   # choose indentation width
```

Formatting is idempotent, and a file with lexer/parser errors is never
rewritten. See [`tools/formatter/README.md`](tools/formatter/README.md).

### Documentation generator: `a doc`

Generates API documentation from `##` doc comments attached to
declarations, with Markdown and structured JSON output.

```a
## Adds two numbers.
fn add(a: i32, b: i32) -> i32:
    return a + b
```

```bash
a doc geometry.a                    # Markdown to stdout
a doc -o docs/geometry.md geometry.a
a doc --json geometry.a             # machine-readable API model
```

Functions, classes/structs, enums, type aliases, fields, methods, variants,
constants, and variables are included. See
[`tools/documentation/README.md`](tools/documentation/README.md).

---

## 🧩 Editor Support

A ships with first-class editor tooling built directly on the compiler front
end, so diagnostics and navigation match `a check`.

### Language server: `a-lsp`

`tools/language-server/` implements the Language Server Protocol over stdio
with no third-party dependencies.

| Capability | What it does |
| :--- | :--- |
| **Diagnostics** | Live lex, parse, and type errors with spans, messages, and compiler help text |
| **Go to definition** | Locals, parameters, functions, structs/classes, fields, methods, enum variants, and imported module members |
| **Find references** | Every use of a symbol, including across imported modules |
| **Hover** | Signatures and types for symbols, plus documentation for builtins, native module members (`json.parse`), and dot methods (`"abc".strip`) |
| **Completion** | Keywords, builtins, visible bindings, module members after `.` — including native modules (`json.`) and the string/array/map/set method sets — plus `import`/`from … import` candidates; docs load on demand via `completionItem/resolve` |
| **Signature help** | Call signature with the active parameter highlighted for user functions, builtins, module members, and dot methods (triggered by `(` and `,`) |
| **Document symbols** | Outline of functions, structs, fields, methods, enums, variants, and top-level bindings |
| **Workspace symbols** | `Ctrl+T` / `Cmd+T` search across open A documents |
| **Formatting** | Full-document formatting through the same AST formatter as `a fmt` |
| **Rename** | Rename a symbol across its declaration and every resolved reference |
| **Document highlights** | Highlight every occurrence of the symbol under the cursor |
| **Folding** | Collapse function, class, loop, match, and conditional bodies |

Build it with the rest of the toolchain:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target a-lsp -j
./build/a-lsp --version
```

### VS Code extension

`tools/vscode-a/` adds TextMate syntax highlighting and connects VS Code to
`a-lsp`:

- Keywords (`var` `const` `pvt` `type` `fn` `struct` `class` `enum` `import` …),
  declarations, types (`Json`, fixed arrays, `T?` optionals), builtins, numbers,
  operators (`..`, `++`, `--`, `|>`, `??`, `?.`), and comments
- Strings with `{interpolation}`, raw `"""` strings, char literals, and escapes
- `#` line comments, bracket matching, auto-closing pairs, completion triggers
  (`.`, `"`, `(`, `,`), and indentation rules for colon-introduced blocks
- Diagnostics, go to definition (`F12`), references (`Shift+F12`), hover,
  completion with documentation on expand, signature help, document outline,
  and workspace symbols
- Format Document (`Shift+Alt+F`), Rename Symbol (`F2`), document
  highlights, and folding ranges

Try it in development:

```bash
# Open tools/vscode-a in VS Code / VS Code Insiders, then press F5.
```

Or install the prebuilt extension from the [GitHub
Release](https://github.com/OEvortex/A/releases) assets:

```bash
# Use whichever CLI you have installed:
code --install-extension a-language-0.0.1.vsix
code-insiders --install-extension a-language-0.0.1.vsix
```

To package it from a checkout instead:

```bash
cd tools/vscode-a
npx --yes @vscode/vsce package
```

The extension finds `a-lsp` in this order:

1. The `a.server.path` setting
2. The `A_LSP_PATH` environment variable
3. `build/a-lsp` or `build-*/a-lsp` inside the workspace
4. `a-lsp` on `PATH`

Useful settings:

| Setting | Default | Purpose |
| :--- | :--- | :--- |
| `a.server.path` | `""` | Absolute path to `a-lsp` |
| `a.server.args` | `[]` | Extra server arguments, such as `["--stdio"]` |
| `a.server.modulePaths` | `[]` | Extra directories for `import` resolution |

Run **A: Restart Language Server** from the command palette after rebuilding
the server. See [`tools/language-server/README.md`](tools/language-server/README.md)
and [`tools/vscode-a/README.md`](tools/vscode-a/README.md) for details.

### Other editors

Any LSP client can use `a-lsp`; start it over stdio and send `file://` document
URIs. Set `A_LSP_LOG=1` for lifecycle logging on stderr.

---

## 🧪 Testing

Run the full test suite (language integration tests, tooling tests, formatter
and doc-generator CLI tests, plus the language server protocol smoke test):

```bash
ctest --test-dir build --output-on-failure
```

Or run the pieces directly:

```bash
tests/integration/run_tests.sh build/a
./build/a-tooling-tests
tests/tooling/run_cli_tests.sh build/a
python3 tools/language-server/smoke_test.py build/a-lsp
```

---

## 📜 License & Acknowledgments

Distributed under the MIT License. See [LICENSE](LICENSE) for details. Detailed specifications and compiler internals can be found in [`docs/`](docs/).
