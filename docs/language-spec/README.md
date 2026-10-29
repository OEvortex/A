# The A Language Reference (v0.3)

A is indentation-based and statically typed. Blocks are introduced by a colon
and indented consistently; braces are not used for blocks.

## Lexical structure

- Comments start with `#` and run to the end of the line.
- Indentation must use spaces; tabs in indentation are reported as errors.
- Identifiers match `[A-Za-z_][A-Za-z0-9_]*`. A bare `_` is a discard name.
- Keywords: `var const pvt type fn if elif else while do for in loop break
  continue return defer struct class metaclass enum import from as match
  self true false and or not None`.
- Numeric literals support `_` separators, decimal integers, fractional parts,
  exponent notation, and hex/binary integer literals (`0xFF`, `0b1010_1011`).
- String literals use double quotes with escapes `\n \t \r \0 \a \b \f \v \\
  \" \{ \xNN \u{...}`. `\xNN` is a two-digit byte escape; `\u{...}` takes one
  to six hex digits and encodes a Unicode code point as UTF-8. A literal
  `{expression}` is interpolated.
- Raw string literals use triple double quotes (`"""..."""`). They span lines,
  perform no escape processing and no interpolation, and strip exactly one
  newline immediately after the opening quotes. Dedenting is not performed.
  A triple-quoted string used as a statement is the idiomatic multi-line
  comment: it is evaluated and discarded.
- Character literals use single quotes, cover one Unicode code point, and
  accept the same escapes as strings.
- Operators (27+): `+ - * / % ** & | ^ ~ << >> .. in not in == != < <= > >= = += -= *= /= %=
  &= |= ^= <<= >>= ++ -- and or not ?. ?? ? : ! => -> |>`.


## Bindings and types

```a
answer = 42              # mutable by default (Python style), inferred as i32
answer = 43              # rebind in place
total += 5               # compound assignment
name: String = "Ada"     # explicit type annotation (also accepts 'str', 'int', 'float')
const PI = 3.14159       # immutable constant
```

The first assignment to an unknown name declares a mutable binding (Python-style variable declaration).
`var` can also be used explicitly. Rebinding and updating in place are fully supported.
To define an immutable value that cannot be reassigned, use `const`:

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

## Control flow

### Conditionals

```a
if score > 90:
    print("A")
elif score > 80:
    print("B")
else:
    print("C")
```

### `while` loop — pre-test

The condition is checked **before** each iteration. The body may run zero times.

```a
count = 0
while count < 3:
    print("while: count = {count}")
    count = count + 1
```

### `do ... while` loop — post-test

The body runs **at least once**; the condition is checked after each iteration.

```a
n = 0
do:
    print("do-while: n = {n}")
    n = n + 1
while n < 2
```

### `for` loop — iteration

Iterates over a range, array, string, or other iterable. Ranges are written
`lo..hi` (exclusive of `hi`) and may include an optional step: `lo..hi..step`.

```a
# range 0..4 → 0, 1, 2, 3
total = 0
for i in 0..4:
    total = total + i
    print("for: i = {i}, total = {total}")

# stepped range 0..10..2 → 0, 2, 4, 6, 8
for i in 0..10..2:
    print("stepped: i = {i}")
```

### `loop` — infinite loop

An unconditional loop with no condition. Exit it with `break` (or `return`).

```a
turns = 0
loop:
    turns = turns + 1
    print("loop: turn = {turns}")
    if turns == 3:
        break
```

### `break` and `continue`

`break` exits the enclosing loop immediately. `continue` skips to the next
iteration. Both work inside `while`, `do ... while`, `for`, and `loop`.

```a
for j in 0..4:
    if j == 2:
        continue
    print("j = {j}")
```

### `++` and `--` — increment / decrement

`++` adds 1 to a numeric variable; `--` subtracts 1. Both come in **prefix**
(`++i`, `--i`) and **postfix** (`i++`, `i--`) forms.

- **Prefix** (`++i`): increments first, then returns the new value.
- **Postfix** (`i++`): returns the old value, then increments.

The operand must be a mutable numeric variable (`var x = ...` or a plain
assignment, which is mutable by default).

```a
i = 0
print(++i)   # 1  (prefix: returns new value)
print(i++)   # 1  (postfix: returns old value)
print(i)     # 2

j = 5
print(--j)   # 4  (prefix: returns new value)
print(j--)   # 4  (postfix: returns old value)
print(j)     # 3
```

They are especially handy in loops:

```a
k = 0
while k < 3:
    print("k = {k}")
    k++
```

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

## Classes, structs, and methods

Both `class` (or `Class`) and `struct` are supported:

```a
class Animal:
    sound: str

    fn speak(self) -> str:
        return self.sound

class Dog(Animal):
    name: str

d = Dog("Woof", "Rex")
print(d.name, "says", d.speak())
```

### Metaclasses and `pass`

```a
metaclass ModelMeta:
    pass

class BaseModel(metaclass=ModelMeta):
    id: int

m = BaseModel(1)
```

Constructors accept either positional arguments or named arguments (mixing is
an error). Methods declare `self` explicitly as the first parameter. Class/struct
values are reference values: passing or assigning one shares the same object.
Derived classes inherit fields and methods from their base class.

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

Methods (Python-compatible names; case conversion and classification are
ASCII-only, and non-ASCII code points pass through unchanged):

| Method | Result |
| ------ | ------ |
| `capitalize()`, `title()`, `swapcase()` | ASCII case conversion |
| `casefold()`, `lower()`, `upper()` | ASCII case conversion |
| `center(width[, fill])`, `ljust(width[, fill])`, `rjust(width[, fill])` | pad to `width` code points |
| `zfill(width)` | zero-pad, keeping a leading `+`/`-` |
| `count(sub[, start[, end]])` | number of non-overlapping matches |
| `find(sub[, start[, end]])`, `rfind(...)` | code-point index of the match, or `-1` |
| `index(sub[, start[, end]])`, `rindex(...)` | code-point index, runtime error when absent |
| `startswith(prefix[, start[, end]])`, `endswith(suffix[, start[, end]])` | prefix/suffix tests |
| `expandtabs([tabsize])` | expand tabs (default 8) |
| `strip([chars])`, `lstrip([chars])`, `rstrip([chars])` | trim characters (default ASCII whitespace) |
| `partition(sep)`, `rpartition(sep)` | `(before, sep, after)` tuple |
| `removeprefix(affix)`, `removesuffix(affix)` | exact prefix/suffix removal |
| `replace(old, new[, count])` | replace occurrences |
| `split([sep[, maxsplit]])`, `rsplit([sep[, maxsplit]])` | `[String]`; without a separator splits on whitespace |
| `splitlines([keepends])` | `[String]` split on line boundaries |
| `join(parts)` | join a `[String]` with the receiver |
| `isalnum()`, `isalpha()`, `isascii()`, `isdecimal()`, `isdigit()`, `isidentifier()`, `islower()`, `isnumeric()`, `isprintable()`, `isspace()`, `istitle()`, `isupper()` | classification tests |
| `contains(sub)` | A extension: substring test |
| `repeat(n)` | A extension: repeat the string `n` times |
| `chars()` | A extension: `[char]` of code points |

Numeric conversion uses the free functions `int(x)` and `float(x)`, which
accept numbers, bools, and numeric strings (surrounding ASCII whitespace and a
leading `+` are allowed) and raise a runtime error on invalid input. There are
no `to_int`/`to_float` methods.

## Arrays

Arrays are reference values, support negative indexing, and carry methods:

| Method | Result |
| ------ | ------ |
| `append(value)` | append, returns `void` |
| `extend(other)` | append every element of another array |
| `insert(index, value)` | insert before `index`; clamps to the valid range |
| `pop([index])` | remove and return the last element (or the given index) |
| `remove(value)` | remove the first equal element, runtime error when absent |
| `clear()` | remove every element |
| `copy()` | shallow copy |
| `count(value)` | number of occurrences |
| `index(value[, start[, end]])` | first index, runtime error when absent |
| `reverse()` | reverse in place |
| `sort()` | sort ints, floats, or strings in place (no `key=`/`reverse=`) |
| `remove_at(index)` | A extension: remove and return the element |
| `contains(value)` | A extension: membership test |
| `first()`, `last()` | A extension: `T?`, `None` when empty |
| `join(sep)` | A extension: concatenate string forms with `sep` |

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
| `int(value)` | `i64` from an int, bool, float (truncated toward zero), or numeric string |
| `float(value)` | `f64` from an int, bool, float, or numeric string |
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

## Tooling and diagnostics

The `a` binary provides `run`, `build`, `check`, `test`, `fmt`, and `doc`.
Diagnostics carry stable codes; warnings never change the exit code.

| Command | Purpose |
| ------- | ------- |
| `a run <file.a>` | compile and execute a program |
| `a build <file.a> [-o out]` | compile to a native binary |
| `a check <file.a>` | type-check without running |
| `a check --json <file.a>` | emit `{"diagnostics":[...]}` as JSON |
| `a test <file.a>` | run every zero-argument `fn test_*()` |
| `a fmt <file.a>` | format source |
| `a doc <file.a>` | generate documentation |

| Code | Warning |
| ---- | ------- |
| `W0001` | unused local variable |
| `W0002` | unreachable statement |

Prefix a local binding with `_` to silence `W0001`. `a test` runs the entry
module's `test_*` functions in name order, prints `ok name` or
`FAILED name: reason`, ends with a `<passed> passed, <failed> failed` summary,
and exits with code 1 when any test fails.

## Not yet supported

Interfaces/traits, `?` error propagation over `Result` types, map and set
values, fixed-size and multidimensional arrays, `i128`/`u128`/`f16`/`bf16`,
package management, and concurrency primitives are future work. Generics,
type aliases, class inheritance, open-ended ranges, and raw triple-quoted
strings are supported.
