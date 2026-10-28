#!/usr/bin/env bash
# CLI coverage for `a fmt` and `a doc`.
#
# Usage: tests/tooling/run_cli_tests.sh [path-to-a-binary]
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
A_BIN="${1:-$ROOT_DIR/build/a}"
if [[ "$A_BIN" != /* ]]; then
  A_BIN="$(cd "$(dirname "$A_BIN")" 2>/dev/null && pwd)/$(basename "$A_BIN")"
fi
if [[ ! -x "$A_BIN" ]]; then
  echo "error: A binary not found at '$A_BIN'" >&2
  exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

SOURCE="$WORK_DIR/sample.a"
cat >"$SOURCE" <<'EOF'
## Adds two numbers.
fn add( a:i32,b:i32 )->i32:
  return a+b

# A note.
var value=0xFF # trailing
EOF

EXPECTED="$WORK_DIR/expected.a"
cat >"$EXPECTED" <<'EOF'
## Adds two numbers.
fn add(a: i32, b: i32) -> i32:
    return a + b

# A note.
var value = 0xFF  # trailing
EOF

"$A_BIN" fmt "$SOURCE" >"$WORK_DIR/formatted.a"
diff -u "$EXPECTED" "$WORK_DIR/formatted.a"

if "$A_BIN" fmt --check "$SOURCE" >/dev/null 2>"$WORK_DIR/check.err"; then
  echo "error: --check accepted an unformatted file" >&2
  exit 1
fi
if ! grep -q "needs formatting" "$WORK_DIR/check.err"; then
  echo "error: --check did not explain the failure" >&2
  exit 1
fi

"$A_BIN" fmt -w "$SOURCE"
"$A_BIN" fmt --check "$SOURCE"
diff -u "$EXPECTED" "$SOURCE"

BROKEN="$WORK_DIR/broken.a"
printf 'fn broken(:\n' >"$BROKEN"
cp "$BROKEN" "$WORK_DIR/broken.original"
if "$A_BIN" fmt -w "$BROKEN" >/dev/null 2>&1; then
  echo "error: fmt accepted a file with a syntax error" >&2
  exit 1
fi
cmp "$BROKEN" "$WORK_DIR/broken.original"

"$A_BIN" doc "$SOURCE" >"$WORK_DIR/sample.md"
grep -q "Adds two numbers." "$WORK_DIR/sample.md"
grep -q 'fn add(a: i32, b: i32) -> i32' "$WORK_DIR/sample.md"

"$A_BIN" doc --json -o "$WORK_DIR/sample.json" "$SOURCE"
python3 - "$WORK_DIR/sample.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
assert data["module"] == "sample", data
assert any(item["name"] == "add" for item in data["items"]), data
PY

echo "OK: fmt and doc CLI"
