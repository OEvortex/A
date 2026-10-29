# binary

Compact self-describing serialization for any A value.

```a
import binary
import json

bytes = binary.encode([1, 2, 3])
back = binary.decode(bytes)!
print(json.as_int((json.at(back, 1))!)!)   # 2
print(binary.decode([200, 1, 2]))          # None
```

| Member | Signature | Result |
| ------ | --------- | ------ |
| `encode` | `(value)` | `[i64]`, one entry per byte (0-255) |
| `decode` | `([i64])` | `Json?` — `None` when the bytes are malformed or truncated |

Values are stored as tag-length-value records with zigzag variable-length
integers. Integers, floats, booleans, characters, strings, arrays, maps, sets,
structs, tuples, optionals and results round-trip exactly. Decoded data
is surfaced as the opaque `Json` value, so use the `json.*` accessors (see the
`json` module) to read it back into typed A values. Values with no portable
representation — closures, allocators, raw pointers and ranges — fail at encode
time with a runtime error.
