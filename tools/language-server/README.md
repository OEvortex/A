# A language server

`a-lsp` is the Language Server Protocol server for the A programming language.
It reuses the compiler front end (`lexer`, `parser`, `module loader`, and
`Sema`), so editor feedback matches `a check`.

## Features

| LSP method | Behaviour |
| ---------- | --------- |
| `textDocument/didOpen`, `didChange`, `didSave`, `didClose` | full-text document sync |
| `textDocument/publishDiagnostics` | lex, parse, and type-check diagnostics |
| `textDocument/definition` | variables, parameters, functions, structs, fields, methods, enum variants, imported modules |
| `textDocument/references` | all uses of the symbol under the cursor, including imported modules |
| `textDocument/hover` | declaration signature/type for symbols and builtins |
| `textDocument/completion` | keywords, builtins, visible locals, module members, struct/enum members after `.` |
| `textDocument/documentSymbol` | functions, structs/classes, fields, methods, enums, variants, and top-level bindings |
| `workspace/symbol` | top-level symbols in open A documents |

The server is single-threaded, speaks JSON-RPC over stdin/stdout, and has no
third-party dependencies.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target a-lsp -j
```

The executable is `build/a-lsp`. The editor extension finds it automatically
when it lives in `build/` (or another `build-*` directory) at the workspace
root. Otherwise set `a.server.path` to its absolute path, or put `a-lsp` on
`PATH`.

Run it directly to check that it starts:

```sh
build/a-lsp --version
```

Editors normally start the server themselves. For manual debugging:

```sh
build/a-lsp --stdio
```

Set `A_LSP_LOG=1` to write lifecycle messages to stderr.

## Layout

- `analysis.hpp` / `analysis.cpp` — symbol index, scopes, definition/reference
  resolution, and compilation wrapper.
- `json.hpp` / `json.cpp` — minimal JSON parser/serializer for LSP messages.
- `server.cpp` — stdio JSON-RPC loop and LSP request handlers.
- `smoke_test.py` — end-to-end protocol smoke test.

## Limitations

- Documents are analysed on every change (full sync). This is fast for normal
  A files, but very large files may benefit from delay/debounce in the editor;
  the VS Code extension debounces changes for 250 ms.
- Type inference results are not shown on hover yet; annotations and declared
  signatures are. The type checker does not expose per-expression types.
- Rename, formatting, code actions, and semantic tokens are not implemented.
  Highlighting is provided by the editor grammar.
- Member completion falls back to all known struct members when the receiver
  type cannot be inferred.
