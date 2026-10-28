# The A Language Reference (v0.1)

A is indentation-based and statically typed. Blocks are introduced by a colon
and indented consistently; braces are not used for blocks.

## Lexical structure

- Comments start with `#` and run to the end of the line.
- Indentation must use spaces; tabs in indentation are reported as errors.
- Identifiers match `[A-Za-z_][A-Za-z0-9_]*`.
- Keywords: `var fn if elif else while for in loop break continue return defer
  struct self true false and or not import`.
- Numeric literals support `_` separators, decimal integers, fractional parts,
  and exponent notation. Hex and binary integer literals are not supported yet.
- String literals use double quotes with escapes `\n \t \r \0 \\ \" \{`. A
  literal `{expression}` is interpolated.
- Character literals use single quotes and currently cover ASCII only.

## Bindings and types

```a
answer = 42            # immutable, inferred as i32
name: String = "Ada"   # explicit annotation
var total = 0          # mutable binding
total += 5             # compound assignment
```

The first assignment to an unknown name declares a binding. Redeclaration in
the same scope is an error. Without `var`, rebinding is rejected. Mutability
controls reassignment only: fields of struct values and elements of arrays can
always be updated.

Built-in types:

| Type | Description |
| ---- | ----------- |
| `bool` | `true` / `false` |
| `char` | one Unicode-aware code point (ASCII literals in v0.1) |
| `i8`, `i16`, `i32`, `i64` | signed integers |
| `u8`, `u16`, `u32`, `u64` | unsigned integers |
| `f32`, `f64` | IEEE floating point |
| `String` | immutable UTF-8 text |
| `[T]` | homogeneous array |
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
errors.

## Expressions

Operators, from lowest to highest precedence:

| Operators | Notes |
| --------- | ----- |
| `or` | short-circuit |
| `and` | short-circuit |
| `not` | prefix |
| `== != < <= > >=` | booleans and comparable values |
| `..` | range constructor (end exclusive) |
| `+ -` | numeric addition/subtraction, string concatenation |
| `* / %` | numeric |
| `-` (prefix) | negation |
| `**` | exponentiation, right-associative |

Both operands of an arithmetic operator must have the same type. `String +
String` concatenates. Ranges are written `start..end` and produce the integers
`start` through `end - 1`. Calls, indexing (`a[i]`), slicing (`a[start..end]`),
and field access (`.field`) are postfix operators and can be chained.

Arrays are written `[1, 2, 3]` and inferred as `[i32]`. Arrays are reference
values; slicing copies. Indexing is zero-based and bounds-checked. Negative
indices are rejected.

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

There are no default arguments, variadic user functions, closures, or
overloading in v0.1.

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

## Strings

Strings are immutable UTF-8 sequences. Literal text and interpolated
expressions can be combined:

```a
total = 3
print("total = {total}, doubled = {total * 2}")
```

Methods:

| Method | Result |
| ------ | ------ |
| `to_int()` | parse an integer, runtime error on invalid input |
| `to_float()` | parse a float, runtime error on invalid input |
| `upper()`, `lower()` | ASCII case conversion |
| `trim()` | strip surrounding ASCII whitespace |
| `contains(sub)` | substring test |
| `split(sep)` | `[String]` of pieces |

Strings cannot be indexed or sliced in v0.1 because byte indexing would split
UTF-8 sequences. Use iteration, `split`, or the methods above.

## Built-in functions

| Name | Description |
| ---- | ----------- |
| `print(values...)` | space-separated values plus newline to stdout |
| `eprint(values...)` | same, to stderr |
| `input(prompt)` | print prompt, read one line as `String` |
| `len(container)` | length of a string (code points) or array |
| `str(value)` | convert any value to its display form |
| `abs(x)` | absolute value |
| `min(a, b)`, `max(a, b)` | matching numeric arguments |
| `pow(base, exp)` | `f64` power |
| `sqrt`, `sin`, `cos`, `tan`, `exp`, `log`, `floor`, `ceil` | `f64` math |
| `i8`...`i64`, `u8`...`u64`, `f32`, `f64` | checked conversions |

`[T]` methods: `push(value)` (returns `void`) and `pop() -> T` (runtime error on
an empty array).

## Runtime behavior

- Integer arithmetic is checked. Overflow, division by zero, out-of-range
  conversions, invalid indices, missing struct fields at runtime, and invalid
  string-to-number conversions produce runtime errors with source locations.
- Runtime errors exit with code `2`; compile errors exit with code `1`.
- Evaluation is depth-first and left-to-right. `print` writes to stdout and
  flushes on process exit.
- Programs in `a run` execute top-level statements in order after all functions
  and structs are registered.

## Not yet supported

`import`, generics, enums, interfaces/traits, closures, `Result`/`Option`,
default parameters, negative indexing, native code generation, and garbage
collection tuning are future work. The compiler reports a clear diagnostic for
syntax it recognizes but does not support yet.
