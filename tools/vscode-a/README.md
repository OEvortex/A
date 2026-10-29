# A Language for VS Code

Syntax highlighting, diagnostics, and language intelligence for the
[A programming language](../../README.md).

## Features

- TextMate syntax highlighting for `.a` files: keywords, declarations, types,
  builtins, numbers, strings, string interpolation, characters, comments, and
  operators.
- Live compiler diagnostics (`a check` semantics) reported as you type.
- Go to definition with `F12` / `Ctrl`-click for locals, parameters, functions,
  structs, fields, methods, enum variants, and imported modules.
- Find all references (`Shift+F12`).
- Hover signatures, builtin documentation, and `##` declaration docs.
- Completion for keywords, builtins, visible bindings, module members, and
  struct/enum members after `.`.
- Document outline and workspace symbol search (`Ctrl+T`).
- Format Document (`Shift+Alt+F`) through `a fmt` semantics.
- Rename Symbol (`F2`) across declarations and references.
- Document highlights for the symbol under the cursor.
- Folding ranges for functions, classes, loops, matches, and conditionals.
- Bracket matching, auto-closing pairs, comments (`#`), and indentation rules.

Language intelligence is provided by `a-lsp` from this repository. The
extension is dependency-free (no `npm install` required) and spawns the server
over stdio.

## Quick start

The quickest path is the prebuilt `a-language-0.0.1.vsix` asset attached to the
[v0.0.1 release](https://github.com/OEvortex/A/releases); install it with
`code --install-extension a-language-0.0.1.vsix` and point `a.server.path` at a
local `a-lsp` build (or keep it on `PATH`).

To run the extension straight from this checkout instead:

1. Build the toolchain and language server from the repository root:

   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --target a-lsp -j
   ```

2. Open this folder (`tools/vscode-a`) in VS Code and press `F5` to launch an
   Extension Development Host. To build an installable package:

   ```sh
   cd tools/vscode-a
   npx @vscode/vsce package
   code --install-extension a-language-0.0.1.vsix
   ```

3. Open any `.a` file. Syntax highlighting works immediately. If `a-lsp` is
   found, diagnostics and navigation start automatically.

## Finding the server

The extension looks for `a-lsp` in this order:

1. `a.server.path` (settings).
2. `A_LSP_PATH` in the environment.
3. `build/a-lsp` and `build-*/a-lsp` inside each workspace folder.
4. `build/a-lsp` at the root of this development checkout.
5. `a-lsp` on `PATH`.

If the server is missing, highlighting still works and a warning explains how
to build it.

## Settings

| Setting | Default | Description |
| ------- | ------- | ----------- |
| `a.server.path` | `""` | Absolute path to `a-lsp`. |
| `a.server.args` | `[]` | Extra arguments, for example `["--stdio"]`. |
| `a.server.modulePaths` | `[]` | Extra import search directories. |

The server also reads `A_PATH` and the repository `stdlib/` directory through
the compiler's normal module resolution.

## Command

- **A: Restart Language Server** — restarts `a-lsp` and reopens documents,
  useful after rebuilding the server.

## Notes

- The server uses full-document synchronization. The extension debounces edits
  by 250 ms before sending them.
- Static highlighting is deliberate: it works without the server and does not
  depend on compiler internals.
- Formatting, rename, highlights, and folding are served by `a-lsp`; rebuilding
  the server is enough to enable them. Use **A: Restart Language Server**
  after an upgrade.
