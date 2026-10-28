# Documentation generator

`a doc` extracts API documentation from A source and renders it as Markdown or
JSON. Documentation comments use `##` and attach to the declaration that
immediately follows them.

```a
## Adds two numbers.
##
## The result is an `i32`.
fn add(a: i32, b: i32) -> i32:
    return a + b
```

## Usage

```sh
# Markdown to stdout
a doc geometry.a

# Write Markdown to a file
a doc -o docs/geometry.md geometry.a

# Structured JSON for custom tooling
a doc --json geometry.a
```

## What is documented

- Functions, including parameters and return types
- Classes, structs, and metaclasses, including fields and methods
- Enums and their variants
- Type aliases
- Top-level constants and variables

Leading `##` comments become the module introduction when they are separated
from the first declaration by a blank line. `#` comments remain ordinary
source comments and are not included in generated docs.

## Markdown structure

Generated Markdown contains a module title, source attribution, an optional
module introduction, a table of contents, and grouped sections for functions,
types, and constants/variables. Every item includes its A signature in a fenced
`a` block followed by its documentation.

## JSON structure

`--json` emits a single object:

```json
{
  "module": "geometry",
  "path": "geometry.a",
  "doc": "",
  "items": [
    {
      "kind": "function",
      "name": "add",
      "signature": "fn add(a: i32, b: i32) -> i32",
      "doc": "Adds two numbers.",
      "returnType": "i32",
      "private": false,
      "params": [{"name": "a", "type": "i32"}],
      "fields": [],
      "variants": [],
      "methods": []
    }
  ]
}
```

## Layout

- `compiler/source/tooling/docgen.cpp` — extraction and Markdown/JSON renderers
- `compiler/source/lexer/lexer.cpp` — `##` comment capture
- `compiler/source/tooling/ast_text.cpp` — shared signature printers
