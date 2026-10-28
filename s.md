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