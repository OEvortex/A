# trace

Nested spans with W3C context propagation, for distributed tracing.

```a
import trace
import json

span = trace.start("request")
trace.set_attribute(span, "http.route", "/home")
child = trace.start("query")        # nested: the parent is the active span
trace.add_event(child, "cache_miss")
trace.end(child)
carrier = trace.inject()            # "00-<32 hex>-<16 hex>-01"
trace.end(span)

for record in trace.spans():
    print((json.as_str((json.get(record, "name"))!))!)
```

| Member | Signature | Result |
| ------ | --------- | ------ |
| `start` | `(name)` | span id (`i64`); becomes the active span and inherits the current one as parent |
| `end` | `(span)` | marks it finished and pops it from the active stack |
| `current` | `()` | active span id, or `0` when nothing is active |
| `set_attribute` | `(span, key, value)` | any A value; later calls with the same key overwrite |
| `add_event` | `(span, name)` | timestamped event on the span |
| `inject` | `()` | `String` traceparent header, or `""` when no span is active |
| `extract` | `(traceparent)` | a new `remote` span continuing the extracted trace |
| `spans` | `()` | `[Json]`, one record per span in creation order |
| `reset` | `()` | forgets every span |

Span records are opaque `Json` maps with the keys `name`, `trace_id`,
`span_id`, `parent_id`, `start_ms`, `end_ms`, `duration_ms`, `attributes` and
`events`; read them with the `json.*` accessors. Ids are random, and times come
from the clock, so only structure and names are stable in golden tests.
`extract` requires a well-formed traceparent
(`00-<32 lowercase hex>-<16 hex>-01`) and reports anything else as a runtime
error. Ending an unknown span id is also a runtime error.
