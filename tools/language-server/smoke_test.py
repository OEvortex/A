#!/usr/bin/env python3
"""End-to-end smoke test for a-lsp.

Usage:
    python3 tools/language-server/smoke_test.py build/a-lsp [--log]
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = """import json

## A two-dimensional point.
struct Point:
    x: i32
    y: i32

    fn length_squared(self) -> i32:
        return self.x * self.x + self.y * self.y

## Adds two numbers.
fn add(a: i32, b: i32) -> i32:
    return a + b

point = Point(3, 4)
total=add(point.x,point.y)
doc = json.parse("[1, 2]")
words = "abc"
print("total = {total}")
print(doc, words.strip())
"""


class Client:
    def __init__(self, server: str) -> None:
        environment = dict(os.environ)
        if "--log" in sys.argv:
            environment["A_LSP_LOG"] = "1"
        self.process = subprocess.Popen(
            [server],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        self.next_id = 1

    def send(self, message: dict) -> None:
        body = json.dumps(message).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        assert self.process.stdin is not None
        self.process.stdin.write(header + body)
        self.process.stdin.flush()

    def receive(self) -> dict:
        assert self.process.stdout is not None
        headers: dict[str, str] = {}
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("a-lsp closed stdout")
            text = line.decode("ascii").strip()
            if not text:
                break
            name, value = text.split(":", 1)
            headers[name.lower()] = value.strip()
        length = int(headers["content-length"])
        return json.loads(self.process.stdout.read(length))

    def request(self, method: str, params: dict) -> dict:
        request_id = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        while True:
            message = self.receive()
            if message.get("id") == request_id and "method" not in message:
                return message

    def notify(self, method: str, params: dict) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def wait_notification(self, method: str) -> dict:
        while True:
            message = self.receive()
            if message.get("method") == method:
                return message

    def stop(self) -> None:
        self.request("shutdown", {})
        self.notify("exit", {})
        self.process.wait(timeout=5)


def fail(message: str) -> None:
    print(f"FAIL: {message}")
    raise SystemExit(1)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    server = sys.argv[1]
    if not Path(server).is_file() and not Path(server).exists():
        print(f"a-lsp executable not found: {server}")
        return 2

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "main.a"
        path.write_text(SOURCE, encoding="utf-8")
        uri = path.as_uri()

        client = Client(server)
        client.request(
            "initialize",
            {
                "rootUri": Path(directory).as_uri(),
                "capabilities": {},
            },
        )
        client.notify("initialized", {})
        client.notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": uri,
                    "languageId": "a",
                    "version": 1,
                    "text": SOURCE,
                }
            },
        )

        diagnostics = client.wait_notification("textDocument/publishDiagnostics")
        if diagnostics["params"]["diagnostics"]:
            fail(f"unexpected diagnostics: {diagnostics['params']['diagnostics']}")

        lines = SOURCE.splitlines()
        call_line = next(i for i, line in enumerate(lines) if "add(point.x" in line)
        call_column = lines[call_line].index("add")
        definition = client.request(
            "textDocument/definition",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
            },
        )
        expected_line = next(i for i, text in enumerate(lines) if text.startswith("fn add("))
        range_ = definition["result"]["range"]
        if range_["start"]["line"] != expected_line:
            fail(
                f"definition for 'add' landed on line {range_['start']['line']}, "
                f"expected {expected_line}"
            )

        hover = client.request(
            "textDocument/hover",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
            },
        )
        hover_text = hover["result"]["contents"]["value"]
        if "fn add" not in hover_text:
            fail("hover did not describe the add function")
        if "Adds two numbers." not in hover_text:
            fail("hover did not include the declaration's doc comment")

        symbols = client.request(
            "textDocument/documentSymbol", {"textDocument": {"uri": uri}}
        )["result"]
        names = {symbol["name"] for symbol in symbols}
        if not {"Point", "add", "point", "total"}.issubset(names):
            fail(f"missing document symbols: {sorted(names)}")

        completion = client.request(
            "textDocument/completion",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column + 1},
            },
        )["result"]["items"]
        labels = {item["label"] for item in completion}
        if not {"add", "Point", "print"}.issubset(labels):
            fail("completion did not include functions, structs, and builtins")

        formatting = client.request(
            "textDocument/formatting",
            {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 4, "insertSpaces": True},
            },
        )["result"]
        if len(formatting) != 1:
            fail(f"formatting returned {len(formatting)} edits, expected 1")
        formatted = formatting[0]["newText"]
        if "total = add(point.x, point.y)" not in formatted:
            fail("formatting did not normalize the assignment")
        if "\n\n\n" in formatted:
            fail("formatting left repeated blank lines")

        rename = client.request(
            "textDocument/rename",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
                "newName": "plus",
            },
        )["result"]
        edits = rename.get("changes", {}).get(uri, [])
        if len(edits) < 2:
            fail(f"rename returned {len(edits)} edits, expected declaration and use")
        if any(edit["newText"] != "plus" for edit in edits):
            fail("rename produced an unexpected replacement")

        highlights = client.request(
            "textDocument/documentHighlight",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
            },
        )["result"]
        if len(highlights) < 2:
            fail(f"document highlight returned {len(highlights)} ranges, expected at least 2")

        folding = client.request(
            "textDocument/foldingRange", {"textDocument": {"uri": uri}}
        )["result"]
        struct_line = next(i for i, text in enumerate(lines) if text.startswith("struct Point:"))
        if not any(entry["startLine"] == struct_line for entry in folding):
            fail(f"folding ranges did not include the struct body: {folding}")

        def position_of(fragment: str) -> tuple[int, int]:
            for i, text in enumerate(lines):
                offset = text.find(fragment)
                if offset >= 0:
                    return i, offset
            raise AssertionError(f"fragment not found in source: {fragment!r}")

        # Native module members complete after `json.`.
        json_line, json_col = position_of("json.parse(")
        items = client.request(
            "textDocument/completion",
            {"textDocument": {"uri": uri}, "position": {"line": json_line, "character": json_col + 5}},
        )["result"]["items"]
        labels = {item["label"] for item in items}
        if not {"parse", "stringify", "as_int"}.issubset(labels):
            fail(f"json. completion missing members: {sorted(labels)[:20]}")

        # Primitive methods complete after a bound string's dot.
        words_line, words_col = position_of("words.strip()")
        items = client.request(
            "textDocument/completion",
            {"textDocument": {"uri": uri}, "position": {"line": words_line, "character": words_col + 6}},
        )["result"]["items"]
        labels = {item["label"] for item in items}
        if not {"strip", "startswith", "join"}.issubset(labels):
            fail(f"string completion missing methods: {sorted(labels)[:20]}")

        # Hover explains module members and primitive methods from the tables.
        _, parse_col = position_of("json.parse(")
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": json_line, "character": parse_col + 6}},
        )["result"]
        if not hover or "json.parse(text)" not in hover["contents"]["value"]:
            fail("hover did not describe json.parse")

        _, strip_col = position_of("words.strip()")
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": words_line, "character": strip_col + 7}},
        )["result"]
        if not hover or "strip([chars])" not in hover["contents"]["value"]:
            fail("hover did not describe the string method strip")

        # Signature help inside `json.parse(` names the parameter.
        signature = client.request(
            "textDocument/signatureHelp",
            {"textDocument": {"uri": uri}, "position": {"line": json_line, "character": parse_col + 11}},
        )["result"]
        if (
            not signature
            or "json.parse(text)" not in signature["signatures"][0]["label"]
            or signature["activeParameter"] != 0
        ):
            fail(f"signature help inside json.parse(: {signature}")

        # After the comma in `add(point.x, ...)` the active parameter advances.
        add_line, add_col = position_of("add(point.x,")
        signature = client.request(
            "textDocument/signatureHelp",
            {
                "textDocument": {"uri": uri},
                "position": {"line": add_line, "character": add_col + len("add(point.x,")},
            },
        )["result"]
        if not signature or signature["activeParameter"] != 1:
            fail(f"signature help after the comma in add(...): {signature}")
        if "fn add(a: i32, b: i32)" not in signature["signatures"][0]["label"]:
            fail(f"signature help did not use the declared signature: {signature}")

        client.stop()
        print(
            "OK: diagnostics, definition, hover, symbols, completion, formatting, "
            "rename, highlights, folding, module/string completion, signature help"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
