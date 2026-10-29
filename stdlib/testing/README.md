# testing

Property-based testing: generate random inputs, check that a property holds for
every one of them, and report the first counterexample.

```a
import testing

testing.seed(42)
ok = testing.for_all_int(200, -1000, 1000, fn(n: i64) -> bool => n + 0 == n)
print(ok)                                 # true

bad = testing.for_all_int(50, 0, 1000, fn(n: i64) -> bool => n < 500)
print(bad)                                # false, counterexample on stderr
```

| Member | Signature |
| ------ | --------- |
| `seed` | `(u64)` — makes a run reproducible; the default seed is time-based |
| `for_all_int` | `(runs, low, high, fn(i64) -> bool)` |
| `for_all_float` | `(runs, low, high, fn(f64) -> bool)` |
| `for_all_bool` | `(runs, fn(bool) -> bool)` |
| `for_all_string` | `(runs, maxLength, fn(String) -> bool)` — printable ASCII |
| `for_all_elem` | `([T], fn(T) -> bool)` — every element of a fixed array |

Each `for_all_*` returns `true` when the property held for every generated
input. On the first failure it writes `Property failed after N test(s): input =
X` to stderr and returns `false`; the program keeps running, so tests can
collect several results. Runs must be non-negative and at most 1,000,000, and
`low <= high`. Use `test.a` (the A-file `test` module) for plain assertions.
