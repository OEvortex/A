# The A Language Reference (v0.2)

A is indentation-based and statically typed. Blocks are introduced by a colon
and indented consistently; braces are not used for blocks.

## Lexical structure

- Comments start with `#` and run to the end of the line.
- Indentation must use spaces; tabs in indentation are reported as errors.
- Identifiers match `[A-Za-z_][A-Za-z0-9_]*`. A bare `_` is a discard name.
- Keywords: `var fn if elif else while for in loop break continue return defer
  struct self true false and or not import match None`.
- Numeric literals support `_` separators, decimal integers, fractional parts,
  exponent notation, and hex/binary integer literals (`0xFF`, `0b1010_1011`).
- String literals use double quotes with escapes `\n \t \r \0 \\ \" \{`. A
  literal `{expression}` is interpolated.
- Raw string literals use triple double quotes (`"""..."""`). They span lines,
  perform no escape processing and no interpolation, and strip exactly one
  newline immediately after the opening quotes. Dedenting is not performed.
- Character literals use single quotes and cover one Unicode code point.
- Operators: `+ - * / % ** .. == != < <= > >= = += -= *= /= %= and or not
  ?. ?? ? ! => -> |>`.

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
| `??` | left | optional fallback, right side is lazy |
| `or` | left | short-circuit |
| `and` | left | short-circuit |
| `not` | prefix | logical negation |
| `== != < <= > >=` | left | comparisons |
| `..` | left | range constructor (end exclusive) |
| `+ -` | left | numeric addition/subtraction, string concatenation |
| `* / %` | left | numeric |
| `-` (prefix) | prefix | negation |
| `**` | right | exponentiation |

Both operands of an arithmetic operator must have the same type. `String +
String` concatenates. Ranges are written `start..end` and produce the integers
`start` through `end - 1`.

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

- Patterns: integer, float, string, char, and bool literals; **inclusive**
  integer ranges (`400..499` covers 499); and the wildcard `_`.
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

for i in 0..10:
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
indexed and tuples cannot be compared with `==` in v0.2; compare their fields
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
- Programs in `a run` execute top-level statements in order after all functions
  and structs are registered.

## Not yet supported

`import`, generics, enums, interfaces/traits, `Result` types, default
parameters, tuple indexing, tuple equality, native code generation, and garbage
collection tuning are future work. The compiler reports a clear diagnostic for
syntax it recognizes but does not support yet.
