<p align="center">
  <h1 align="center"><b>The A Programming Language</b></h1>
  <p align="center">
    <strong>Fast as C++. Expressive as Python. Statically typed. Memory safe.</strong>
  </p>
  <p align="center">
    <a href="#benchmarks">Benchmarks</a> •
    <a href="#quickstart">Quickstart</a> •
    <a href="#language-tour">Language Tour</a> •
    <a href="#features">Features</a> •
    <a href="#documentation">Docs</a>
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

## 🛠️ Quickstart

### Prerequisites
* CMake 3.20+
* C++20 compiler (GCC 11+, Clang 14+, or MSVC 19.29+)

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

# 3. Compile to optimized standalone native binary:
./build/a build program.a -o my_app
./my_app
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

## 📂 Project Organization

```
├── ac                          # Instant CLI runner script
├── compiler/
│   ├── source/
│   │   ├── lexer/              # Indentation-aware lexer & tokenization
│   │   ├── parser/             # Recursive-descent parser
│   │   ├── ast/                # Typed AST nodes
│   │   ├── type-checker/       # Global Sema & unification type inference
│   │   ├── backend/
│   │   │   ├── interpreter.cpp # High-speed tagged development interpreter
│   │   │   └── native/         # Native AOT codegen hitting C++ parity
│   │   └── cli/                # Command line interface (`a run`, `a build`)
├── runtime/                    # Values, memory models, UTF-8 strings, and file I/O
├── stdlib/                     # Standard library modules (math, fs, io, net)
├── benchmarks/                 # Verification suites with cross-language equivalents
├── examples/                   # Curated code examples
└── tests/                      # Automated golden test suite
```

---

## 🧪 Testing

Run the full integration test suite:
```bash
ctest --test-dir build --output-on-failure
# Or directly:
tests/integration/run_tests.sh build/a
```

---

## 📜 License & Acknowledgments

Distributed under the MIT License. See [LICENSE](LICENSE) for details. Detailed specifications and compiler internals can be found in [`docs/`](docs/).
