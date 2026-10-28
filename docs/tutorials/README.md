# Tutorials

Start with the language reference at [docs/language-spec](../language-spec/README.md)
and the programs under `examples/`.

Suggested order:

1. `examples/hello-world` — printing.
2. `examples/input-output` — reading stdin.
3. `examples/loops` — `while`, `for`, ranges, `do ... while`, and `++`/`--`.
4. `examples/functions` — functions, closures, `match`, and pipelines.
5. `examples/collections` — arrays, strings, comprehensions.
6. `examples/structs` — structs, methods, and optionals.
7. `examples/modules` — multi-file programs with imports.

Each directory contains a small program you can run directly:

```sh
build/a run examples/modules/app.a
```

## Tour: modules

`examples/modules/app.a` imports helper modules, binds a namespace alias, and
pulls individual names into scope:

```a
import "lib/cart.a" as cart
from "lib/money.a" import of

basket = cart.empty()
basket.add("coffee", of(4, 50))
basket.render()
```

Imported modules run once, before the importer, and their top-level bindings
stay live. Run the example from any directory: quoted paths resolve relative
to the importing file, not the current directory.

## Tour: enums, bitwise, and ternary

```a
enum Level:
    Low
    Medium = 5
    High

fn describe(level: Level) -> String:
    return match level:
        Level.Low => "low"
        Level.Medium => "medium"
        _ => "high"

flags = 0b1010 & 0b0110      # 0b0010
shifted = 1 << 4             # 16
cleared = ~shifted           # -17

print(describe(Level.Medium))
print(flags | 1)             # 3
print(cleared < 0 ? "negative" : "positive")
```

Enum variants auto-increment, `const` declares named immutable values, and
`do ... while` guarantees at least one iteration.

## Tour: closures

```a
fn make_shifter() -> fn(i32) -> i32:
    var bump = 1
    shift = fn(n: i32) => n + bump
    bump = 100     # not observed: locals are captured by value
    return shift

shifted = make_shifter()
print(shifted(1))   # 2

base = 10           # module global: resolved live at call time
add_base = fn(n: i32) => n + base
print(add_base(5))  # 15
```

Lambdas capture locals by value and stop at their module boundary; globals are
resolved live at each call. Captured arrays and structs are references, so
mutations through them are visible.
