# Formatter

`a fmt` is an AST-based source formatter for the A programming language. It is
implemented in `compiler/source/tooling/formatter.cpp` and shared by the CLI
and the language server, so `a fmt` and “Format Document” produce identical
output.

## Usage

```sh
# Print formatted source to stdout
a fmt program.a

# Check formatting in CI; exits 1 when the file needs changes
a fmt --check program.a

# Rewrite the file in place
a fmt -w program.a

# Use a different indentation width
a fmt -w --indent 2 program.a
```

The formatter never writes a file when the lexer or parser reports an error;
`--check` therefore cannot hide broken code.

## What it normalises

- 4-space indentation (configurable) and consistent blank lines
- Spacing around operators, commas, colons, and `=`/`->`/`=>`
- Canonical `var` / `const` / `pvt` / `class` / `struct` / `metaclass` keywords
- Function signatures, generics, parameter lists, and return types
- `if` / `elif` / `else`, loops, `match`, structs, enums, and type aliases
- Literal spelling is preserved (`0xFF`, `1_000`, escape sequences, raw strings)
- Comments are preserved, including leading, trailing, and comments inside
  multiline expressions
- Long calls and array literals are wrapped one argument/element per line

Formatting is idempotent: formatting already formatted output is a no-op.

## Layout

- `compiler/source/tooling/formatter.cpp` — source formatter
- `compiler/source/tooling/source_text.cpp` — span-to-text mapping
- `compiler/source/tooling/ast_text.cpp` — shared type/signature printers

## Tests

`tests/tooling/tooling_tests.cpp` covers canonical output, idempotence, comment
preservation, literal preservation, and parse-error handling.
`tests/tooling/run_cli_tests.sh` verifies `a fmt`, `--check`, `-w`, and the
“never clobber broken files” guarantee end to end.
