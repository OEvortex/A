# Standard library

Standard library modules come in two forms:

- **Native modules** implemented by the toolchain.
- **A-file modules** under `stdlib/`, resolvable with `import "..."` or bare
  names such as `import test`.

Usage:

```a
import math
from math import sqrt, PI

print(math.floor(3.7))
print(sqrt(PI))
```

Module resolution searches the importing file's directory, `-I` paths,
`A_PATH`, and the stdlib root (or `--stdlib`). Details:
[docs/language-spec](../language-spec/README.md#modules).

## Native modules

| Module | Members |
| ------ | ------- |
| `math` | `sqrt abs min max pow sin cos tan asin acos atan atan2 exp log log2 log10 floor ceil round trunc random random_int`, constants `PI E` |
| `io` | `print eprint input flush` |
| `fs` | `read_file write_file append_file file_exists list_dir remove_file create_dir remove_dir rename current_dir` |
| `strings` | `len str join` |
| `collections` | `len map filter reduce any all sum join` |
| `core` | `panic assert assert_eq` |
| `time` | `now sleep` |

- `fs` paths are strings. `list_dir` returns sorted entry names, `create_dir`
  creates missing parents, and `remove_dir` only removes empty directories.
  Failures are runtime errors that include the operating-system message.
- `now()` returns Unix epoch milliseconds; `sleep(ms)` pauses for a
  non-negative duration.
- `assert` and `assert_eq` raise runtime errors; `panic(message)` aborts
  unconditionally. All three exit with status 2.
- `net` and `json` are reserved directories with no runtime support yet.

## A-file modules

`stdlib/test/test.a` is a small assertion harness built on `core`:

```a
import test

test.check("addition", 1 + 1 == 2)
test.equal("double", 6, 3 * 2)
test.require(true, "always true")
test.report()               # "2 passed, 0 failed"
```

The module exports the live counters `test.passed` and `test.failed`, plus
`check`, `equal`, `require`, `summary`, and `report`.
