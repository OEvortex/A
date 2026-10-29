# log

Structured logging to stderr. Records are either human-readable or single-line
JSON, and a level threshold filters out anything below it.

```a
import log

log.set_level("info")            # debug | info | warn | error
log.set_json(false)              # true emits one JSON object per line
log.info("user signup", "user_id", 42, "plan", "pro")
log.warn("slow request", {"route": "/home", "ms": "850"})
log.error("boom", "code", 500, "tags", ["a", "b"])
```

| Member | Signature |
| ------ | --------- |
| `set_level` | `(String)` — `debug`, `info`, `warn`, or `error` |
| `set_json` | `(bool)` — output format |
| `debug` / `info` / `warn` / `error` | `(message[, fields])` |

Fields may be given as one map (or struct) or as alternating `name, value`
pairs; anything else is a runtime error. The default format is

```
2026-01-31T12:00:00.000Z INFO user signup user_id=42 plan="pro"
```

and the JSON format adds `timestamp`, `level`, and `message` keys before the
fields, one record per line. Field names that collide with those three keys are
dropped from JSON output. Timestamps are UTC with millisecond precision, so
golden tests should match stable substrings instead of whole lines.
