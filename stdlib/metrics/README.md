# metrics

A process-local metrics registry that renders the Prometheus text exposition
format.

```a
import metrics

metrics.counter("http_requests", 2, "route", "/home", "method", "GET")
metrics.gauge("queue_depth", 7.5)
metrics.histogram("latency_ms", 12.5, {"route": "/home"})
print(metrics.render())
```

| Member | Signature |
| ------ | --------- |
| `counter` | `(name, increment[, labels])` — adds a non-negative integer |
| `gauge` | `(name, value[, labels])` — sets the current value |
| `histogram` | `(name, value[, labels])` — records one observation |
| `render` | `() -> String` — Prometheus text format |
| `reset` | `()` — forgets every series |

Labels are either one map (or struct) or alternating `name, value` pairs; values
are converted with the same rules as log fields and are sorted by label name, so
`{"a": 1, "b": 2}` and `("b", 2, "a", 1)` describe the same series. Registering
the same name with a different metric type is a runtime error.

Rendered output contains one `# TYPE` line per metric family, then one line per
series; histogram series expand into cumulative `_bucket` lines (with an `le`
label), plus `_sum` and `_count`. Buckets are the Prometheus defaults:
`0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, +Inf`.

```
# TYPE http_requests counter
http_requests{method="GET",route="/home"} 2
```

`render` is a plain string, so a program can serve it from an HTTP handler:

```a
fn handler(method: String, path: String, headers: {String: String}, body: String) -> (i64, {String: String}, String):
    if path == "/metrics":
        return (200, {"Content-Type": "text/plain; version=0.0.4"}, metrics.render())
    return (200, {"Content-Type": "text/plain"}, "ok")
```
