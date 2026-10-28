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

SOURCE = """struct Point:
    x: i32
    y: i32

    fn length_squared(self) -> i32:
        return self.x * self.x + self.y * self.y

fn add(a: i32, b: i32) -> i32:
    return a + b

point = Point(3, 4)
total = add(point.x, point.y)
print("total = {total}")
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
        call_line = next(i for i, line in enumerate(lines) if "total = add(" in line)
        call_column = lines[call_line].index("add")
        definition = client.request(
            "textDocument/definition",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
            },
        )
        range_ = definition["result"]["range"]
        if range_["start"]["line"] != 7:
            fail(f"definition for 'add' landed on line {range_['start']['line']}, expected 7")

        hover = client.request(
            "textDocument/hover",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
            },
        )
        if "fn add" not in hover["result"]["contents"]["value"]:
            fail("hover did not describe the add function")

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

        client.stop()
        print("OK: diagnostics, definition, hover, document symbols, completion")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
