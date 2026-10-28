# The A Language Reference (v0.3)

A is indentation-based and statically typed. Blocks are introduced by a colon
and indented consistently; braces are not used for blocks.

## Lexical structure

- Comments start with `#` and run to the end of the line.
- Indentation must use spaces; tabs in indentation are reported as errors.
- Identifiers match `[A-Za-z_][A-Za-z0-9_]*`. A bare `_` is a discard name.
- Keywords: `var const fn if elif else while do for in loop break continue
  return defer struct enum self true false and or not import from as match
  None`.
- Numeric literals support `_` separators, decimal integers, fractional parts,
  exponent notation, and hex/binary integer literals (`0xFF`, `0b1010_1011`).
- String literals use double quotes with escapes `\n \t \r \0 \a \b \f \v \\
  \" \{ \xNN \u{...}`. `\xNN` is a two-digit byte escape; `\u{...}` takes one
  to six hex digits and encodes a Unicode code point as UTF-8. A literal
  `{expression}` is interpolated.
- Raw string literals use triple double quotes (`"""..."""`). They span lines,
  perform no escape processing and no interpolation, and strip exactly one
  newline immediately after the opening quotes. Dedenting is not performed.
- Character literals use single quotes, cover one Unicode code point, and
  accept the same escapes as strings.
- Operators: `+ - * / % ** & | ^ ~ << >> .. == != < <= > >= = += -= *= /= %=
  &= |= ^= <<= >>= and or not ?. ?? ? : ! => -> |>`.


## Bindings and types

```a
answer = 42              # immutable, inferred as i32
name: String = "Ada"     # explicit annotation
var total = 0            # mutable binding
total += 5               # compound assignment
```

The first assignment to an unknown name declares a binding. Redeclaration in
the same scope is an error. Without `var`, rebinding is rejected. Mutability
controls reassignment only: fields of struct values and elements of arrays can
always be updated.

`const NAME = expr` declares a named immutable value, valid at module and local
scope. Constants cannot be reassigned (`Cannot assign to constant 'NAME'.`) and
are initialized in order at runtime.

```a
const LIMIT = 10
const NAME: String = "A"
```

Built-in types:

| Type | Description |
| ---- | ----------- |
| `bool` | `true` / `false` |
| `char` | one Unicode code point |
| `i8`, `i16`, `i32`, `i64` | signed integers |
| `u8`, `u16`, `u32`, `u64` | unsigned integers |
| `f32`, `f64` | IEEE floating point |
| `String` | immutable UTF-8 text |
| `[T]` | homogeneous array |
| `(T, U, ...)` | tuple |
| `fn(T, U) -> R` | function value |
| `T?` | optional value (either `Some(T)` or `None`) |
| `void` | absence of a value |

Integer literals default to `i32` and float literals to `f64`. Context pushes
types inward, so `var x: u64 = 1` checks the literal as `u64`. There are no
implicit numeric conversions: `f64(3)`, `i64(2.9)` (truncating), and the other
conversion functions perform explicit, range-checked conversions.

### Inference

Function parameters and return types may be omitted and are inferred from use:

```a
fn twice(n):
    return n * 2

print(twice(21))
```

If a type variable is still unresolved after analysis, the compiler reports an
inference error and asks for an annotation. Types that are used inconsistently
(for example, passing a `String` where an integer is required) are compile
errors. A bare `None` is allowed wherever the element type can be inferred
later; otherwise annotate the binding (`var x: i32? = None`).

## Expressions

Operators, from lowest to highest precedence:

| Operators | Associativity | Notes |
| --------- | ------------- | ----- |
| `\|>` | left | pipeline: `value \|> f(args)` calls `f(value, args)` |
| `?:` | right | conditional expression, branches are lazy |
| `??` | left | optional fallback, right side is lazy |
| `or` | left | short-circuit |
| `and` | left | short-circuit |
| `not` | prefix | logical negation |
| `== != < <= > >=` | left | comparisons |
| `..` | left | range constructor (end exclusive, optional step) |
| `\|` | left | bitwise or |
| `^` | left | bitwise xor |
| `&` | left | bitwise and |
| `<< >>` | left | shifts |
| `+ -` | left | numeric addition/subtraction, string concatenation |
| `* / %` | left | numeric |
| `- ~` (prefix) | prefix | negation, bitwise not |
| `**` | right | exponentiation |

Both operands of an arithmetic operator must have the same type. `String +
String` concatenates. Ranges are written `start..end` and produce the integers
`start` through `end - 1`; `start..end..step` steps by a positive amount.

Bitwise operators work on integers only. Shifts require an amount from 0 to 63;
`<<` wraps on 64 bits and `>>` is arithmetic. `~x` equals `-x - 1`.

The conditional expression `cond ? a : b` requires a `bool` condition, both
branches must unify to one type, and only the selected branch is evaluated:

```a
grade = score >= 90 ? "A" : "B"
label = n % 2 == 0 ? "even" : "odd"
```


Postfix operators: calls, indexing (`a[i]`), slicing (`a[start..end]`,
`a[start..end..step]`), field access (`.field`), safe navigation (`a?.field`),
and unwrap (`a!`).

Arrays are written `[1, 2, 3]` and inferred as `[i32]`. Arrays are reference
values; slicing copies. Indexing is zero-based and bounds-checked. Negative
indices count from the end; negative slice bounds are handled the same way.

### Pipelines

`lhs |> f(args)` parses as `f(lhs, args)`. The right side must be a call with a
plain function name; any other form is a compile error. Continuation lines of a
multi-line pipeline may start with `|>`:

```a
total = values
    |> filter(fn(n) => n > 0)
    |> map(fn(n) => n * 2)
    |> sum()
```

### Lambdas

`fn(params) => expression` creates a function value. Parameters may be typed and
an optional `-> T` return annotation is allowed. The body is a single
expression.

```a
double = fn(n) => n * 2
add = fn(a: i32, b: i32) -> i32 => a + b
print((fn() => 99)())
```

Closures capture visible locals **by value at creation time**. Bindings
reassigned afterwards are not observed; arrays and structs remain shared
references. Global names are resolved at call time. Named functions are
first-class values as well (`map(values, double)`).

### Comprehensions

`[expression for name in iterable]`, optionally with a condition:

```a
squares = [n * n for n in 0..10]
evens = [n for n in values if n % 2 == 0]
letters = [c for c in "hello"]
```

The iterable may be a range, an array (iterated over a snapshot), or a string
(code points). The loop variable is scoped to the comprehension and immutable.

### Match expressions

`match` is an expression; each arm maps a pattern to a result expression:

```a
desc = match status:
    200 => "OK"
    400..499 => "Client error"
    _ if status < 0 => "Invalid"
    _ => "Other"
```

- Patterns: integer, float, string, char, and bool literals; enum variants
  (`Color.Red`); **inclusive** integer ranges (`400..499` covers 499); and the
  wildcard `_`.
- An optional `if guard` follows the pattern.
- Every arm must produce the same type.
- A match must end with an unguarded `_` arm so every value is handled.
- Note that match ranges are inclusive, unlike expression ranges.

## Optionals

`None` is the empty optional; `Some(x)` wraps a value:

```a
var maybe: i32? = None
print(maybe ?? 0)      # 0; the fallback is evaluated lazily
maybe = Some(41)
print(maybe! + 1)      # unwraps; runtime error on None
```

- `a ?? b`: `a` must be optional; `b` unifies with the element type and is only
  evaluated when needed.
- `a!`: unwraps an optional; a `None` produces the runtime error
  "Cannot unwrap None with '!'."
- `x?.field`: safe navigation on an optional struct. The result is optional,
  and an optional field is flattened, so
  `user?.address?.city` yields `String?`.
- `==`/`!=` between optionals is allowed when the element is a primitive,
  `String`, `char`, `bool`, or another optional (`None == x` works).

## Statements

```a
if condition:
    ...
elif other:
    ...
else:
    ...

while condition:
    ...

do:
    ...                 # runs at least once, then re-checks condition
while condition

for i in 0..10:
    ...

for i in 0..10..2:      # stepped range: 0, 2, 4, 6, 8
    ...

for item in array:      # iterates over a snapshot of the array
    ...

for ch in "text":       # iterates over code points
    ...

loop:
    break               # exit the innermost loop
    continue            # next iteration

defer print("cleanup")  # runs when the enclosing block exits
```

`do` loops take a colon after `do` and no colon after `while`. A stepped range
requires a positive step; slice syntax (`a[0..n..2]`) is unchanged.

`defer` callbacks in the same block run in reverse (LIFO) order. They still run
when the block exits early through `return`, `break`, `continue`, or a runtime
error.

## Functions

```a
fn add(a: i32, b: i32) -> i32:
    return a + b

fn greet(name: String) -> void: print("Hello, {name}")
```

- Functions may call functions declared later in the file.
- `void` functions may omit `return`; returning a value from them is an error.
- Non-void functions must return on every path.
- Single-line bodies are allowed after `:`.
- Top-level `fn` and `struct` declarations do not need to precede their use.

There are no default arguments, variadic user functions, or overloading.

## Modules

A program may span multiple files. Imports are only legal at the top level of
a module:

```a
import "lib/mathx.a"                 # namespace = stem: mathx.add(...)
import "lib/mathx.a" as mx           # namespace = mx
from "lib/mathx.a" import add, Vec   # bind names directly

import math                          # native module
from math import sqrt, PI
```

- Quoted paths resolve against the importing file's directory, then each `-I`
  directory, then `A_PATH`, then the standard library directory.
- Bare names resolve to native modules first (`math`, `io`, `fs`, `strings`,
  `collections`, `core`, `time`), then `stdlib/<name>.a` or
  `stdlib/<name>/<name>.a`.
- Every module runs its top-level statements **exactly once**, in dependency
  order (imports before importers). Circular imports are a compile error.
- Exports are functions, structs, enums, constants, and module-level `var`
  bindings. `m.name` reads a live binding and `m.name = value` writes it;
  functions are called as `m.fn(...)`, structs as `m.Point(...)`, and enum
  variants as `m.Color.Red`.
- `from ... import` binds names in the importer's scope. Imports are not
  transitive: importing a module does not expose that module's own imports.
- Functions and lambdas see the globals of the module where they are defined.

## Tuples

Tuples group fixed-length heterogeneous values:

```a
fn divmod(a: i32, b: i32) -> (i32, i32):
    return (a / b, a % b)

(q, r) = divmod(17, 5)       # immutable destructuring
var (lo, hi) = (1, 2)        # mutable destructuring
(_, rem) = divmod(9, 4)      # '_' discards a slot
print((1, "two", true))      # (1, "two", true)
```

Destructuring requires a tuple of the same arity. Tuple slots cannot be
indexed and tuples cannot be compared with `==` in v0.3; compare their fields
instead.

## Structs and methods

```a
struct Point:
    x: i32
    y: i32

    fn sum(self) -> i32:
        return self.x + self.y

    fn shift(self, dx: i32, dy: i32) -> void:
        self.x += dx
        self.y += dy

p = Point(3, 4)             # positional
q = Point(y=10, x=20)       # named, all-or-nothing
p.shift(1, 1)
print(p.sum())
```

Constructors accept either positional arguments or named arguments (mixing is
an error). Methods declare `self` explicitly as the first parameter. Struct
values are reference values: passing or assigning one shares the same object.
A field holding a function value can be called like a method.

## Enums

Enums are C-style named integer constants backed by `i32`:

```a
enum Color:
    Red              # 0
    Green = 5        # explicit value
    Blue             # 6, the previous value plus one

var c: Color = Color.Blue
print(Color.Green)   # 5
```

- Variants start at `0` and auto-increment; an explicit value resets the
  counter.
- Variant values must be integer literals (optionally negated).
- An enum-typed binding behaves like `i32`, so arithmetic and comparisons work.
- Enum values work in `match` patterns (`Color.Red => ...`).
- Enums declared in a module are exported: `m.Color.Red`.

## Strings

Strings are immutable UTF-8 sequences. Literal text and interpolated
expressions can be combined:

```a
total = 3
print("total = {total}, doubled = {total * 2}")
```

Strings are indexed and sliced by **code point**:

```a
print("héllo"[1])        # é
print("héllo"[0..5])     # héllo
print("abcdef"[0..6..2]) # ace
```

Methods:

| Method | Result |
| ------ | ------ |
| `to_int()` | parse an integer, runtime error on invalid input |
| `to_float()` | parse a float, runtime error on invalid input |
| `try_to_int()` | `i32?`, `None` when the text is not an integer |
| `try_to_float()` | `f64?`, `None` when the text is not a float |
| `upper()`, `lower()` | ASCII case conversion |
| `trim()` | strip surrounding ASCII whitespace |
| `contains(sub)` | substring test |
| `starts_with(sub)`, `ends_with(sub)` | prefix/suffix tests |
| `find(sub)` | code-point index of the first match, or `-1` |
| `replace(old, new)` | replace every occurrence |
| `repeat(n)` | repeat the string `n` times |
| `split(sep)` | `[String]` of pieces |
| `chars()` | `[char]` of code points |
| `join` | arrays only, see below |

## Arrays

Arrays are reference values, support negative indexing, and carry methods:

| Method | Result |
| ------ | ------ |
| `push(value)` | append, returns `void` |
| `pop()` | remove and return the last element |
| `insert(index, value)` | insert before `index`, returns `void` |
| `remove_at(index)` | remove and return the element |
| `reverse()` | reverse in place |
| `sort()` | sort ints, floats, or strings in place |
| `contains(value)` | membership test |
| `index_of(value)` | first index or `-1` |
| `first()`, `last()` | `T?`, `None` when empty |
| `join(sep)` | concatenate string forms with `sep` |

Slices use exclusive end bounds and an optional positive step:
`items[0..10..2]`. A non-positive step is a runtime error.

## Built-in functions

| Name | Description |
| ---- | ----------- |
| `print(values...)` | space-separated values plus newline to stdout |
| `eprint(values...)` | same, to stderr |
| `flush()` | flush stdout and stderr |
| `input(prompt?)` | print prompt, read one line as `String` |
| `len(container)` | length of a string (code points) or array |
| `str(value)` | convert any value to its display form |
| `abs(x)` | absolute value |
| `min(a, b)`, `max(a, b)` | matching numeric arguments |
| `pow(base, exp)` | `f64` power |
| `sqrt`, `sin`, `cos`, `tan`, `exp`, `log`, `log2`, `log10` | `f64` math |
| `floor`, `ceil`, `round`, `trunc` | `f64` rounding |
| `atan2(y, x)` | `f64` angle |
| `random()` | `f64` in `[0, 1)` |
| `random_int(lo, hi)` | `i32` in `[lo, hi]`, inclusive |
| `map(array, fn)` | `[U]` from applying the function |
| `filter(array, fn)` | elements where the predicate is true |
| `reduce(array, init, fn)` | left fold |
| `any(array, fn)`, `all(array, fn)` | `bool` aggregates |
| `sum(array)` | numeric or string concatenation, `0` when empty |
| `join(array, sep)` | concatenate string forms with `sep` |
| `Some(value)` | wrap a value in an optional |
| `read_file(path)` | entire file as `String` |
| `write_file(path, text)` | overwrite a file |
| `append_file(path, text)` | append to a file |
| `file_exists(path)` | `bool` |
| `list_dir(path)` | sorted `[String]` of directory entry names |
| `remove_file(path)` | delete a file |
| `create_dir(path)` | create a directory and missing parents |
| `remove_dir(path)` | delete an empty directory |
| `rename(from, to)` | move or rename a path |
| `current_dir()` | absolute current working directory |
| `now()` | wall-clock time as Unix epoch milliseconds |
| `sleep(ms)` | pause for a non-negative number of milliseconds |
| `panic(message)` | abort with a runtime error |
| `assert(condition, message?)` | runtime error unless the condition is true |
| `assert_eq(expected, actual, message?)` | runtime error unless the values match |
| `i8`...`i64`, `u8`...`u64`, `f32`, `f64` | checked conversions |

Built-in constants: `PI`, `E`, `I8_MIN`/`I8_MAX` through `I64_MIN`/`I64_MAX`,
`U8_MAX`, `U16_MAX`, `U32_MAX`, `F32_MIN`/`F32_MAX`, `F64_MIN`/`F64_MAX`.
`F32_MIN` and `F64_MIN` are the most negative values. There is no `U64_MAX`
because integers are stored in a signed 64-bit representation.

## Runtime behavior

- Integer arithmetic is checked. Overflow, division by zero, out-of-range
  conversions, invalid indices, unsafe unwraps, non-positive slice steps,
  missing struct fields at runtime, and invalid string-to-number conversions
  produce runtime errors with source locations.
- File I/O uses binary mode; failures report the operating-system error text.
  `input` strips a trailing carriage return (CRLF input) and treats end of
  input as a runtime error.
- Runtime errors exit with code `2`; compile errors exit with code `1`.
- Evaluation is depth-first and left-to-right. `print` writes to stdout and
  flushes on process exit or when `flush()` is called.
- Programs in `a run` execute imported modules once (in dependency order), then
  the entry module's top-level statements. Functions and structs are registered
  before any statements run.

## Standard library modules

`import math` and friends expose builtins as namespaces. The available native
modules are `math`, `io`, `fs`, `strings`, `collections`, `core`, and `time`;
the full member list is in
[docs/standard-library](../standard-library/README.md). Modules may also be
written in A under `stdlib/`, such as `stdlib/test/test.a`.

## Not yet supported

Generics, interfaces/traits, `Result` types, default parameters, tuple
indexing, tuple equality, native code generation, and garbage collection tuning
are future work. The compiler reports a clear diagnostic for syntax it
recognizes but does not support yet.
