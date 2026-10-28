#!/usr/bin/env bash
# Time the programs under benchmarks/ with the A interpreter.
#
# Usage: tools/benchmark-runner/run.sh [path-to-a-binary]
# Environment: RUNS (default 3) sets the repetitions; the best time is kept.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
A_BIN="${1:-$ROOT_DIR/build/a}"
RUNS="${RUNS:-3}"

if [[ ! -x "$A_BIN" ]]; then
  echo "error: A binary not found at '$A_BIN'" >&2
  exit 1
fi

now_ms() {
  date +%s%3N
}

printf '%-28s %10s %10s\n' "benchmark" "best (ms)" "output"
for source in "$ROOT_DIR"/benchmarks/*/*.a; do
  [[ -f "$source" ]] || continue
  name="${source#"$ROOT_DIR"/benchmarks/}"
  best=""
  output=""
  for ((run = 0; run < RUNS; ++run)); do
    start="$(now_ms)"
    output="$("$A_BIN" run "$source")"
    elapsed=$(( $(now_ms) - start ))
    if [[ -z "$best" || $elapsed -lt $best ]]; then
      best=$elapsed
    fi
  done
  printf '%-28s %10d %10s\n' "$name" "$best" "$output"
done
