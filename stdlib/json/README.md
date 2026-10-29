# json

JSON parsing and serialization. Values live in the opaque `Json` type: you can
hold them in variables, name the type in a signature (`fn f(doc: Json) -> Json?`),
pass them to `json.*` functions, and convert them back to A values with the
`as_*` accessors, but `Json` has no operators of its own.

```a
import json

doc = json.parse("{\"name\": \"Ada\", \"scores\": [1, 2, 3]}")
print(json.kind(doc))                                  # object
print(json.as_str((json.get(doc, "name"))!)!)          # Ada
print(json.as_int((json.at((json.get(doc, "scores"))!, 1))!)!)  # 2
print(json.stringify(doc))                             # {"name":"Ada","scores":[1,2,3]}
print(json.pretty([1, 2, 3]))                          # multi-line, two-space indent
```

## Members

| Member | Signature | Result |
| ------ | --------- | ------ |
| `parse` | `(String)` | `Json`, runtime error on malformed input |
| `stringify` | `(value)` | compact JSON text for any A value |
| `pretty` | `(value)` | indented JSON text |
| `is_valid` | `(String)` | `bool`, no exception |
| `kind` | `(Json)` | `null`, `bool`, `int`, `float`, `string`, `array`, `object` |
| `has` | `(Json, String)` | key presence |
| `get` | `(Json, String)` | `Json?` value for an object key |
| `at` | `(Json, i64)` | `Json?` value for an array index |
| `len` | `(Json)` | members of an array or object, characters of a string |
| `keys` | `(Json)` | object keys in document order |
| `as_str` / `as_int` / `as_float` / `as_bool` | `(Json)` | `T?`, `None` when the kind differs |
| `as_array` | `(Json)` | `[Json]?` |
| `as_map` | `(Json)` | `{String: Json}?` |

`as_int` accepts integral JSON numbers, `as_float` accepts any JSON number, and
`as_str` also accepts a single-character value. Serializing converts A structs to
JSON objects, arrays, views, sets and tuples to JSON arrays, `Some(x)`/`Ok(x)` to
their inner value, and `None` to `null`; a `Result` is written as an object
with an `ok` flag and its `value`. Closures, allocators and raw pointers cannot be
serialized and raise a runtime error, as does parsing text that is not a single
JSON document.
