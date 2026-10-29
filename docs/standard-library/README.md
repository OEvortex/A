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
| `collections` | `len map filter reduce any all sum join set` |
| `unsafe` | `get set address_of offset load store` — unchecked element access and raw pointers |
| `arena` | `new pool region alloc release reset used total` |
| `core` | `panic assert assert_eq` |
| `time` | `now sleep` |
| `json` | `parse stringify pretty is_valid get at len has keys kind as_str as_int as_float as_bool as_array as_map` |
| `binary` | `encode decode` |
| `testing` | `seed for_all_int for_all_float for_all_bool for_all_string for_all_elem` |
| `log` | `set_level set_json debug info warn error` |
| `metrics` | `counter gauge histogram render reset` |
| `trace` | `start end current set_attribute add_event inject extract spans reset` |
| `net` | `resolve tcp_listen tcp_connect accept send recv close local_port set_timeout set_nonblocking poll udp_socket udp_bind udp_send udp_recv tls_connect tls_available` |
| `http` | `get post request serve serve_once` |

- `fs` paths are strings. `list_dir` returns sorted entry names, `create_dir`
  creates missing parents, and `remove_dir` only removes empty directories.
  Failures are runtime errors that include the operating-system message.
- `now()` returns Unix epoch milliseconds; `sleep(ms)` pauses for a
  non-negative duration.
- `assert` and `assert_eq` raise runtime errors; `panic(message)` aborts
  unconditionally. All three exit with status 2.
- The serialization, observability, testing, networking, and HTTP modules are
  implemented in the interpreter runtime. Programs that use them run with
  `a run` (and `a test`); `a build` reports that a compiled backend cannot
  lower them. Each module has its own notes:
  [json](../../stdlib/json/README.md),
  [binary](../../stdlib/binary/README.md),
  [testing](../../stdlib/testing/README.md),
  [log](../../stdlib/log/README.md),
  [metrics](../../stdlib/metrics/README.md),
  [trace](../../stdlib/trace/README.md),
  [net](../../stdlib/net/README.md),
  [http](../../stdlib/http/README.md).
- `json.parse` yields an opaque `Json` value. `Json` has no operators; read it
  with `json.get`, `json.at`, `json.kind`, and the `as_*` accessors, which
  return optionals. `binary.decode` yields `Json?` and reports malformed input
  as `None` instead of failing.
- `log` writes to stderr, `metrics.render` returns Prometheus text, and
  `trace` records spans in the process. Their output contains timestamps and
  random identifiers, so golden tests should match stable substrings.
- `net` sockets are `i64` handles that must be closed with `net.close`, and TLS
  requires the toolchain to be built with OpenSSL (`net.tls_available` reports
  whether it is). `http` is layered on `net` and handles one request per
  connection on a single thread.
- `arena` allocators return integer byte-offset handles. `new` is a bump
  allocator, `pool` reuses released blocks, and `region` releases in reverse
  allocation order. Out-of-space allocations and out-of-order region releases
  are runtime errors. See
  [Memory allocators](../language-spec/README.md#memory-allocators).

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

## Examples

Runnable programs live in `examples/stdlib/` and run with `a run` (the
compiled backends do not lower these modules):

| File | Shows |
| ---- | ----- |
| `builtin_modules.a` | every builtin package in one script, with no `fn main()` |
| `serialization.a` | parsing, reading, pretty-printing, and a binary round trip |
| `property_testing.a` | seeded properties over ints, floats, bools, strings, and elements |
| `observability.a` | structured log records, a Prometheus render, and span propagation |
| `networking.a` | a loopback TCP exchange, an HTTP handler, and a UDP round trip |
| `ip_lookup.a` | `http.get` against a public API to read this host's IP addresses (needs network access) |
| `strings_methods.a` | the Python-style dot methods on strings, arrays, maps, and sets |
| `memory_arena.a` | arena allocators alongside `unsafe` pointer arithmetic |
