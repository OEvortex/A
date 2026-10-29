#pragma once

#include <string>

#include "collections/transparent_map.hpp"

namespace a {

// Documentation for every interpreter builtin that a native module or the
// bare language surface can reach. Keyed by builtin name (the value that
// `nativeFunction()` maps a module member to), so the type checker, the
// language server, and signature help all read the same table.
struct BuiltinDoc {
  const char* name;
  const char* signature;
  const char* documentation;
};

inline const StringMap<BuiltinDoc>& builtinDocs() {
  static const StringMap<BuiltinDoc> docs = {
      // math
      {"sqrt", {"sqrt", "sqrt(x) -> f64", "Square root of x."}},
      {"abs", {"abs", "abs(x) -> number", "Absolute value, keeping the argument type."}},
      {"min", {"min", "min(a, b) -> number", "The smaller of two values of the same numeric type."}},
      {"max", {"max", "max(a, b) -> number", "The larger of two values of the same numeric type."}},
      {"pow", {"pow", "pow(base, exponent) -> f64", "Raises base to the exponent."}},
      {"sin", {"sin", "sin(x) -> f64", "Sine of x, in radians."}},
      {"cos", {"cos", "cos(x) -> f64", "Cosine of x, in radians."}},
      {"tan", {"tan", "tan(x) -> f64", "Tangent of x, in radians."}},
      {"asin", {"asin", "asin(x) -> f64", "Arc sine, returning radians."}},
      {"acos", {"acos", "acos(x) -> f64", "Arc cosine, returning radians."}},
      {"atan", {"atan", "atan(x) -> f64", "Arc tangent, returning radians."}},
      {"atan2", {"atan2", "atan2(y, x) -> f64", "Angle of the vector (x, y) in radians."}},
      {"exp", {"exp", "exp(x) -> f64", "e raised to the x."}},
      {"log", {"log", "log(x) -> f64", "Natural logarithm."}},
      {"log2", {"log2", "log2(x) -> f64", "Base-2 logarithm."}},
      {"log10", {"log10", "log10(x) -> f64", "Base-10 logarithm."}},
      {"floor", {"floor", "floor(x) -> f64", "Rounds down to the nearest integer value."}},
      {"ceil", {"ceil", "ceil(x) -> f64", "Rounds up to the nearest integer value."}},
      {"round", {"round", "round(x) -> f64", "Rounds to the nearest integer value."}},
      {"trunc", {"trunc", "trunc(x) -> f64", "Truncates toward zero."}},
      {"random", {"random", "random() -> f64", "A pseudo-random float in [0, 1)."}},
      {"random_int", {"random_int", "random_int(low, high) -> i64",
                      "A pseudo-random integer between low and high, inclusive."}},
      {"PI", {"PI", "PI: f64", "The ratio of a circle's circumference to its diameter."}},
      {"E", {"E", "E: f64", "Euler's number, the base of the natural logarithm."}},

      // io
      {"print", {"print", "print(value)", "Writes a value to standard output, followed by a newline."}},
      {"eprint", {"eprint", "eprint(value)", "Writes a value to standard error, followed by a newline."}},
      {"input", {"input", "input([prompt]) -> String", "Reads one line from standard input."}},
      {"flush", {"flush", "flush()", "Flushes buffered standard output."}},

      // fs
      {"read_file", {"read_file", "read_file(path) -> String", "Reads an entire text file."}},
      {"write_file", {"write_file", "write_file(path, text)", "Creates or replaces a file with text."}},
      {"append_file", {"append_file", "append_file(path, text)", "Appends text to a file."}},
      {"file_exists", {"file_exists", "file_exists(path) -> bool", "Whether a path exists."}},
      {"list_dir", {"list_dir", "list_dir(path) -> [String]", "Sorted entry names of a directory."}},
      {"remove_file", {"remove_file", "remove_file(path)", "Deletes a file."}},
      {"create_dir", {"create_dir", "create_dir(path)", "Creates a directory and any missing parents."}},
      {"remove_dir", {"remove_dir", "remove_dir(path)", "Removes an empty directory."}},
      {"rename", {"rename", "rename(old, new)", "Renames or moves a path."}},
      {"current_dir", {"current_dir", "current_dir() -> String", "The process working directory."}},

      // strings / collections (shared builtins)
      {"len", {"len", "len(value) -> i64", "Length of a string, array, map, or set."}},
      {"str", {"str", "str(value) -> String", "Converts a value to its string representation."}},
      {"join", {"join", "join(values, separator) -> String",
                "Concatenates an array of strings with a separator."}},
      {"map", {"map", "map(values, fn) -> array", "Applies a function to every element."}},
      {"filter", {"filter", "filter(values, fn) -> array",
                  "Keeps the elements for which a predicate is true."}},
      {"reduce", {"reduce", "reduce(values, initial, fn) -> value",
                  "Folds an array from left to right."}},
      {"any", {"any", "any(values, fn) -> bool", "True when the predicate holds for some element."}},
      {"all", {"all", "all(values, fn) -> bool", "True when the predicate holds for every element."}},
      {"sum", {"sum", "sum(values) -> number", "Sum of an array of numbers."}},
      {"set", {"set", "set(values) -> Set",
                "Builds a set from an array, range, set, or string."}},

      // unsafe
      {"unsafe_get", {"unsafe_get", "unsafe_get(array, index) -> T",
                       "Unchecked element access; no bounds check."}},
      {"unsafe_set", {"unsafe_set", "unsafe_set(array, index, value)",
                       "Unchecked element assignment; no bounds check."}},
      {"unsafe_address_of", {"unsafe_address_of", "unsafe_address_of(array, index) -> *T",
                             "Raw pointer to an array element."}},
      {"unsafe_load", {"unsafe_load", "unsafe_load(pointer) -> T",
                       "Reads the value a pointer refers to."}},
      {"unsafe_store", {"unsafe_store", "unsafe_store(pointer, value)",
                        "Writes through a raw pointer."}},
      {"unsafe_offset", {"unsafe_offset", "unsafe_offset(pointer, count) -> *T",
                         "Moves a pointer by a number of elements."}},

      // arena
      {"arena_new", {"arena_new", "arena_new(capacity) -> Allocator",
                     "Bump allocator: allocations are fast, release is all-or-nothing."}},
      {"arena_pool", {"arena_pool", "arena_pool(capacity) -> Allocator",
                      "Pool allocator: released blocks are reused for equal sizes."}},
      {"arena_region", {"arena_region", "arena_region(capacity) -> Allocator",
                        "Region allocator: releases must happen in reverse allocation order."}},
      {"arena_alloc", {"arena_alloc", "arena_alloc(allocator, size) -> i64",
                        "Allocates bytes and returns a handle; errors when out of space."}},
      {"arena_release", {"arena_release", "arena_release(allocator, handle)",
                          "Returns an allocation to the allocator."}},
      {"arena_reset", {"arena_reset", "arena_reset(allocator)",
                        "Frees every allocation the allocator owns."}},
      {"arena_used", {"arena_used", "arena_used(allocator) -> i64",
                      "Bytes currently handed out."}},
      {"arena_total", {"arena_total", "arena_total(allocator) -> i64",
                        "Bytes of capacity."}},

      // core
      {"panic", {"panic", "panic(message)", "Aborts with exit status 2 and the message."}},
      {"assert", {"assert", "assert(condition[, message])",
                  "Runtime error when the condition is false."}},
      {"assert_eq", {"assert_eq", "assert_eq(actual, expected[, message])",
                     "Runtime error when the two values differ."}},

      // time
      {"now", {"now", "now() -> i64", "Current time as Unix epoch milliseconds."}},
      {"sleep", {"sleep", "sleep(milliseconds)", "Pauses for a non-negative duration."}},

      // conversions / optionals
      {"int", {"int", "int(value) -> i64", "Converts a number, bool, or numeric string to i64."}},
      {"float", {"float", "float(value) -> f64",
                  "Converts a number, bool, or numeric string to f64."}},
      {"Some", {"Some", "Some(value) -> T?", "Wraps a value in an optional."}},
      {"Ok", {"Ok", "Ok(value) -> Result", "Wraps a value as a successful result."}},
      {"Err", {"Err", "Err(error) -> Result", "Wraps a value as a failed result."}},
      {"move", {"move", "move(value) -> T", "Moves ownership out of a binding."}},
      {"copy", {"copy", "copy(value) -> T", "Deep-copies a value."}},
      {"borrow", {"borrow", "borrow(variable)", "Immutable borrow tracked by the analyzer."}},
      {"borrow_mut", {"borrow_mut", "borrow_mut(variable)",
                      "Mutable borrow tracked by the analyzer."}},

      // json
      {"json_parse", {"json_parse", "json.parse(text) -> Json",
                       "Parses one JSON document; malformed input is a runtime error."}},
      {"json_stringify", {"json_stringify", "json.stringify(value) -> String",
                           "Compact JSON text for any serializable A value."}},
      {"json_pretty", {"json_pretty", "json.pretty(value) -> String",
                        "Indented JSON text with two-space indent."}},
      {"json_is_valid", {"json_is_valid", "json.is_valid(text) -> bool",
                          "Whether the text parses, without raising errors."}},
      {"json_get", {"json_get", "json.get(doc, key) -> Json?",
                     "Value for an object key, or None."}},
      {"json_at", {"json_at", "json.at(doc, index) -> Json?",
                    "Value at an array index, or None."}},
      {"json_len", {"json_len", "json.len(doc) -> i64",
                     "Members of an array or object, characters of a string."}},
      {"json_has", {"json_has", "json.has(doc, key) -> bool", "Key presence in an object."}},
      {"json_keys", {"json_keys", "json.keys(doc) -> [String]", "Object keys in document order."}},
      {"json_type", {"json_type", "json.kind(doc) -> String",
                      "One of null, bool, int, float, string, array, object."}},
      {"json_as_str", {"json_as_str", "json.as_str(doc) -> String?",
                        "The JSON string, or None when the kind differs."}},
      {"json_as_int", {"json_as_int", "json.as_int(doc) -> i64?",
                        "An integral JSON number as i64, or None."}},
      {"json_as_float", {"json_as_float", "json.as_float(doc) -> f64?",
                          "Any JSON number as f64, or None."}},
      {"json_as_bool", {"json_as_bool", "json.as_bool(doc) -> bool?", "A JSON bool, or None."}},
      {"json_as_array", {"json_as_array", "json.as_array(doc) -> [Json]?", "A JSON array, or None."}},
      {"json_as_map", {"json_as_map", "json.as_map(doc) -> {String: Json}?",
                        "A JSON object as a map, or None."}},

      // binary
      {"binary_encode", {"binary_encode", "binary.encode(value) -> [i64]",
                          "Self-describing bytes, one entry per byte (0-255)."}},
      {"binary_decode", {"binary_decode", "binary.decode(bytes) -> Json?",
                          "Decodes bytes; malformed or truncated input yields None."}},

      // testing
      {"testing_seed", {"testing_seed", "testing.seed(value)",
                        "Fixes the generator so a run is reproducible."}},
      {"testing_for_all_int", {"testing_for_all_int",
                                "testing.for_all_int(runs, low, high, fn(i64) -> bool) -> bool",
                                "Random int property; the first counterexample goes to stderr."}},
      {"testing_for_all_float", {"testing_for_all_float",
                                  "testing.for_all_float(runs, low, high, fn(f64) -> bool) -> bool",
                                  "Random float property; the first counterexample goes to stderr."}},
      {"testing_for_all_bool", {"testing_for_all_bool",
                                 "testing.for_all_bool(runs, fn(bool) -> bool) -> bool",
                                 "Random bool property over generated inputs."}},
      {"testing_for_all_string", {"testing_for_all_string",
                                   "testing.for_all_string(runs, maxLength, fn(String) -> bool) -> bool",
                                   "Random printable-ASCII string property."}},
      {"testing_for_all_elem", {"testing_for_all_elem",
                                 "testing.for_all_elem(values, fn(T) -> bool) -> bool",
                                 "Checks the property for every element of an array."}},

      // log
      {"log_set_level", {"log_set_level", "log.set_level(level)",
                          "One of debug, info, warn, or error."}},
      {"log_set_json", {"log_set_json", "log.set_json(bool)",
                         "True writes one JSON object per line."}},
      {"log_debug", {"log_debug", "log.debug(message[, fields])",
                      "Debug-level record to stderr."}},
      {"log_info", {"log_info", "log.info(message[, fields])", "Info-level record to stderr."}},
      {"log_warn", {"log_warn", "log.warn(message[, fields])", "Warning record to stderr."}},
      {"log_error", {"log_error", "log.error(message[, fields])", "Error record to stderr."}},

      // metrics
      {"metrics_counter", {"metrics_counter", "metrics.counter(name, increment[, labels])",
                            "Adds a non-negative integer to a counter."}},
      {"metrics_gauge", {"metrics_gauge", "metrics.gauge(name, value[, labels])",
                          "Sets a gauge to the value."}},
      {"metrics_histogram", {"metrics_histogram", "metrics.histogram(name, value[, labels])",
                              "Records one observation in a histogram."}},
      {"metrics_render", {"metrics_render", "metrics.render() -> String",
                           "Prometheus text exposition format."}},
      {"metrics_reset", {"metrics_reset", "metrics.reset()", "Forgets every series."}},

      // trace
      {"trace_start", {"trace_start", "trace.start(name) -> i64",
                        "Begins a span nested under the active one; returns its id."}},
      {"trace_end", {"trace_end", "trace.end(span)", "Finishes a span and pops it from the stack."}},
      {"trace_current", {"trace_current", "trace.current() -> i64",
                          "The active span id, or 0 when nothing is active."}},
      {"trace_set_attribute", {"trace_set_attribute", "trace.set_attribute(span, key, value)",
                                "Attaches an attribute; the same key overwrites."}},
      {"trace_add_event", {"trace_add_event", "trace.add_event(span, name)",
                            "Timestamped event on a span."}},
      {"trace_inject", {"trace_inject", "trace.inject() -> String",
                         "traceparent header for the active trace, or \"\"."}},
      {"trace_extract", {"trace_extract", "trace.extract(traceparent) -> i64",
                          "Continues a trace from a W3C traceparent header."}},
      {"trace_spans", {"trace_spans", "trace.spans() -> [Json]",
                        "One record per span in creation order."}},
      {"trace_reset", {"trace_reset", "trace.reset()", "Forgets every span."}},

      // net
      {"net_resolve", {"net_resolve", "net.resolve(host) -> [String]",
                       "Numeric addresses in resolver order."}},
      {"net_tcp_listen", {"net_tcp_listen", "net.tcp_listen(host, port) -> i64",
                           "Listening socket; port 0 asks the OS for one."}},
      {"net_tcp_connect", {"net_tcp_connect", "net.tcp_connect(host, port) -> i64",
                            "Connected TCP socket."}},
      {"net_accept", {"net_accept", "net.accept(listener) -> i64",
                       "Next incoming connection; blocks unless a timeout is set."}},
      {"net_send", {"net_send", "net.send(handle, data) -> i64", "Bytes written."}},
      {"net_recv", {"net_recv", "net.recv(handle, maxBytes) -> String",
                     "Up to maxBytes bytes; empty at EOF or when nothing is waiting."}},
      {"net_close", {"net_close", "net.close(handle)", "Releases the socket."}},
      {"net_local_port", {"net_local_port", "net.local_port(handle) -> i64", "The bound port."}},
      {"net_set_timeout", {"net_set_timeout", "net.set_timeout(handle, millis)",
                            "Send/receive timeout; 0 clears it."}},
      {"net_set_nonblocking", {"net_set_nonblocking", "net.set_nonblocking(handle, bool)",
                                "Non-blocking mode."}},
      {"net_poll", {"net_poll", "net.poll(handles, timeoutMillis) -> [i64]",
                     "Handles that are ready to read."}},
      {"net_udp_socket", {"net_udp_socket", "net.udp_socket() -> i64", "Unbound datagram socket."}},
      {"net_udp_bind", {"net_udp_bind", "net.udp_bind(handle, host, port)",
                         "Binds a datagram socket; port 0 is ephemeral."}},
      {"net_udp_send", {"net_udp_send", "net.udp_send(handle, host, port, data) -> i64",
                         "Bytes sent in one datagram."}},
      {"net_udp_recv", {"net_udp_recv", "net.udp_recv(handle, maxBytes) -> (String, String, i64)",
                         "Payload, peer host, and peer port; (\"\", \"\", -1) when idle."}},
      {"net_tls_connect", {"net_tls_connect", "net.tls_connect(host, port) -> i64",
                            "TLS client socket; requires a build with OpenSSL."}},
      {"net_tls_available", {"net_tls_available", "net.tls_available() -> bool",
                              "Whether the toolchain was built with TLS support."}},

      // http
      {"http_get", {"http_get", "http.get(url) -> (i64, {String: String}, String)",
                     "GET request; returns status, headers, and body."}},
      {"http_post", {"http_post", "http.post(url, body) -> (i64, {String: String}, String)",
                      "POST request; returns status, headers, and body."}},
      {"http_request", {"http_request",
                         "http.request(socket, method, path, headers, body) -> (i64, {String: String}, String)",
                         "One request over an established net socket."}},
      {"http_serve", {"http_serve", "http.serve(listener, handler)",
                       "Serves connections until a handler returns status 0."}},
      {"http_serve_once", {"http_serve_once", "http.serve_once(listener, handler) -> bool",
                            "Handles one connection; true when it was served."}},
  };
  return docs;
}

inline const BuiltinDoc* findBuiltinDoc(const std::string& name) {
  const auto found = builtinDocs().find(name);
  return found == builtinDocs().end() ? nullptr : &found->second;
}

}  // namespace a
