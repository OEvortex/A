# http

A small HTTP/1.1 client and server built on the `net` module.

```a
import http
import net

fn handler(method: String, path: String, headers: {String: String},
           body: String) -> (i64, {String: String}, String):
    if path == "/ping":
        return (200, {"Content-Type": "text/plain"}, "pong " + method)
    return (404, {"Content-Type": "text/plain"}, "missing")

listener = net.tcp_listen("127.0.0.1", 8080)
http.serve(listener, handler)
```

| Member | Signature | Result |
| ------ | --------- | ------ |
| `get` | `(url)` | `(status, headers, body)` |
| `post` | `(url, body)` | `(status, headers, body)` |
| `request` | `(socket, method, path, headers, body)` | `(status, headers, body)` over an established `net` socket |
| `serve` | `(listener, handler)` | handles connections until a handler returns status `0` |
| `serve_once` | `(listener, handler)` | handles one connection, returns `true` if it was served |

Responses are a tuple of the status code, the header map, and the body string.
Header names are lower-cased in the map that `request`/`get`/`post` return, and
lookups of a missing key are a runtime error, so check with `contains` first.
URLs accept `http://host[:port]/path` and `https://host[:port]/path`; HTTPS needs
a build with TLS support (see `net.tls_available`). Requests always send
`Host`, and a `Content-Length` is added when the handler does not set one.

On the server side, request header names are also lower-cased for the handler.
Request bodies may use `Content-Length` or `Transfer-Encoding: chunked`; chunked
bodies are reassembled before the handler runs. A handler can stream its own
chunked response by setting `Transfer-Encoding: chunked` in the header map —
then no `Content-Length` is written and the body is emitted as one chunk
followed by the terminating zero-length chunk. Any other response is sent with
`Content-Length` and `Connection: close`, so each connection carries one
request. Returning status `0` from a handler stops `http.serve`; it closes the
connection and produces no response.

`http.serve` and `http.serve_once` are single-threaded and handle one request
per connection, in the order connections arrive — enough for tests, local tooling
and small services. Failures (a malformed request line, a handler that returns the
wrong tuple shape, a socket error) are runtime errors that exit with status 2.
