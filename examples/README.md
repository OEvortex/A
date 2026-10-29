# Examples

Every program here is self-contained and runs as a script — an A module body
*is* the program, so there is no `fn main()` requirement. From the repository
root:

```sh
./build/a run examples/hello-world/hello.a   # or: ./ac examples/hello-world/hello.a
```

Run them all — skipping the network-dependent lookup, the stdin-driven greeter,
and the deliberately slow throughput demo — with:

```sh
printf 'Ada\n' | ./build/a run examples/input-output/greet.a   # needs stdin
for f in examples/*/*.a; do
  case "$f" in
    examples/stdlib/ip_lookup.a|examples/loops/count_to_billion.a|examples/input-output/greet.a) continue ;;
  esac
  ./build/a run "$f" >/dev/null || echo "FAIL $f"
done
```

| File | Shows |
| ---- | ----- |
| `hello-world/hello.a` | the smallest program: string interpolation |
| `input-output/greet.a` | `input()` and prompt/response flow |
| `loops/fizzbuzz.a` | `for`, `if/elif/else`, modulo |
| `loops/count_to_billion.a` | tight numeric loop — a throughput demo that takes minutes under the interpreter |
| `loops/demo_all_loops.a` | `while`, `do…while`, `for` over ranges, and `break`/`continue` |
| `loops/demo_inc_dec.a` | `++`/`--` and compound assignment |
| `loops/primes.a` | nested loops and early exit |
| `functions/fibonacci.a` | recursion |
| `functions/operators.a` | the operator surface: arithmetic, bitwise, `in`, `|>` |
| `functions/modern_features.a` | single-line expression functions, hex/binary literals, negative indexing and slicing |
| `functions/match_http.a` | `match` expressions classifying HTTP status codes |
| `classes/classes.a` | classes, inheritance, methods, metaclasses |
| `collections/pipeline.a` | array/map methods and `|>` pipelines over collections |
| `modules/app.a` | importing the local `modules/lib` package (`money`, `cart`) |
| `stdlib/builtin_modules.a` | a tour of every native module in one script |
| `stdlib/serialization.a` | `json` parsing/reading/pretty-printing and a `binary` round trip |
| `stdlib/property_testing.a` | seeded `testing.for_all_*` properties |
| `stdlib/observability.a` | structured `log` records, a `metrics` render, `trace` spans |
| `stdlib/networking.a` | loopback TCP, an HTTP handler, and a UDP round trip (binds local sockets) |
| `stdlib/ip_lookup.a` | `http.get` against a public API — **needs network access** |
| `stdlib/strings_methods.a` | the Python-style dot methods on strings, arrays, maps, and sets |
| `stdlib/memory_arena.a` | bump/pool/region arenas and `unsafe` element access |
| `testing/deferred_cleanup.a` | run with `a test` instead of `a run`: `defer` cleanup plus the `test` harness |

Interpreter-backed modules (json, binary, testing, log, metrics, trace, net,
http) run with `a run` and `a test`; `a build` reports when the compiled
backend cannot lower them. See
[`docs/standard-library`](../docs/standard-library/README.md) for the full
member tables.
