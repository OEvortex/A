#!/usr/bin/env bash
# Golden-file test runner for the A toolchain.
#
# Usage: tests/run_tests.sh [path-to-a-binary]
#
# Each tests/cases/<name>.a is executed. Optional companion files:
#   <name>.out    expected stdout (default: empty)
#   <name>.in     stdin fed to the program
#   <name>.err    substrings that must appear in stderr (one per line)
#   <name>.exit   expected exit code (default: 0)
#   <name>.flags  invocation flags; "check" runs 'a check' instead of 'a run'
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
A_BIN="${1:-$ROOT_DIR/build/a}"

if [[ ! -x "$A_BIN" ]]; then
  echo "error: A binary not found at '$A_BIN'" >&2
  echo "build it first: cmake -S . -B build && cmake --build build" >&2
  exit 1
fi

CASES_DIR="$SCRIPT_DIR/cases"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

pass=0
fail=0
failed_names=()

for source in "$CASES_DIR"/*.a; do
  [[ -e "$source" ]] || continue
  name="$(basename "$source" .a)"
  base="$CASES_DIR/$name"

  command="run"
  if [[ -f "$base.flags" ]] && grep -q "check" "$base.flags"; then
    command="check"
  fi

  expected_exit=0
  if [[ -f "$base.exit" ]]; then
    expected_exit="$(tr -d '[:space:]' < "$base.exit")"
  fi

  stdin_file=/dev/null
  [[ -f "$base.in" ]] && stdin_file="$base.in"

  "$A_BIN" "$command" "$source" <"$stdin_file" \
    >"$WORK_DIR/$name.stdout" 2>"$WORK_DIR/$name.stderr"
  actual_exit=$?

  problems=()

  if [[ "$actual_exit" != "$expected_exit" ]]; then
    problems+=("exit code: expected $expected_exit, got $actual_exit")
  fi

  expected_out="$WORK_DIR/$name.expected"
  : >"$expected_out"
  [[ -f "$base.out" ]] && cp "$base.out" "$expected_out"
  if ! diff -u "$expected_out" "$WORK_DIR/$name.stdout" >"$WORK_DIR/$name.diff"; then
    problems+=("stdout mismatch")
  fi

  if [[ -f "$base.err" ]]; then
    while IFS= read -r needle; do
      [[ -z "$needle" ]] && continue
      if ! grep -Fq -- "$needle" "$WORK_DIR/$name.stderr"; then
        problems+=("stderr missing: $needle")
      fi
    done < "$base.err"
  fi

  if [[ ${#problems[@]} -eq 0 ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed_names+=("$name")
    echo "FAIL $name"
    for problem in "${problems[@]}"; do
      echo "  - $problem"
    done
    if [[ -s "$WORK_DIR/$name.diff" ]]; then
      sed 's/^/  /' "$WORK_DIR/$name.diff"
    fi
    if [[ -s "$WORK_DIR/$name.stderr" ]]; then
      sed 's/^/  stderr| /' "$WORK_DIR/$name.stderr"
    fi
  fi
done

echo
echo "passed: $pass, failed: $fail"
[[ $fail -eq 0 ]]
