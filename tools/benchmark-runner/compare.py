#!/usr/bin/env python3
"""Compare A against C++ and Python on the benchmarks/ suite.

Measures end-to-end wall time (best of N runs) for the same algorithm in each
language and prints a table. The C++ equivalent is compiled once with -O2.

Usage:
    python3 tools/benchmark-runner/compare.py [--a build/a] [--runs 5]
                                             [--json results.json]
                                             [--skip-cpp] [--skip-python]

Requires: a built `a` binary, g++ (for C++), and python3.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BENCHES = {
    "arithmetic/sum": {
        "a": "benchmarks/arithmetic/sum.a",
        "py": "benchmarks/equivalents/python/sum.py",
        "cpp": "arithmetic",
    },
    "collections/pipeline": {
        "a": "benchmarks/collections/pipeline.a",
        "py": "benchmarks/equivalents/python/pipeline.py",
        "cpp": "collections",
    },
    "memory/strings": {
        "a": "benchmarks/memory/strings.a",
        "py": "benchmarks/equivalents/python/strings.py",
        "cpp": "memory",
    },
    "numerical/sqrt": {
        "a": "benchmarks/numerical/sqrt.a",
        "py": "benchmarks/equivalents/python/sqrt.py",
        "cpp": "numerical",
    },
}


def time_cmd(cmd: list[str], runs: int) -> tuple[float, float, str]:
    """Return (best_ms, median_ms, stdout) for `runs` invocations."""
    times: list[float] = []
    output = ""
    for _ in range(runs):
        start = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, text=True)
        times.append((time.perf_counter() - start) * 1000.0)
        output = proc.stdout.strip()
        if proc.returncode != 0:
            raise RuntimeError(
                f"command failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stderr}"
            )
    return min(times), statistics.median(times), output


def build_cpp(outdir: str) -> str | None:
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        print("warning: no C++ compiler found, skipping C++", file=sys.stderr)
        return None
    src = os.path.join(ROOT, "benchmarks", "equivalents", "cpp", "bench.cpp")
    out = os.path.join(outdir, "bench-cpp")
    subprocess.run(
        [compiler, "-O2", "-std=c++20", src, "-o", out], check=True, capture_output=True
    )
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", default=os.path.join(ROOT, "build", "a"))
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--json", default="")
    parser.add_argument("--skip-cpp", action="store_true")
    parser.add_argument("--skip-python", action="store_true")
    args = parser.parse_args()

    if not os.access(args.a, os.X_OK):
        print(f"error: A binary not found at {args.a}", file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp(prefix="a-bench-")
    cpp_bin = None if args.skip_cpp else build_cpp(tmp)
    py = sys.executable if not args.skip_python else None

    results = {}
    header = f"{'benchmark':26s} {'A ms':>9s}"
    if py:
        header += f" {'Python ms':>10s}"
    if cpp_bin:
        header += f" {'C++ ms':>8s}"
    header += f" {'A/C++':>8s}  output"
    print(header)
    print("-" * len(header))

    for name, cfg in BENCHES.items():
        row = {}
        a_best, a_med, a_out = time_cmd([args.a, "run", os.path.join(ROOT, cfg["a"])], args.runs)
        row["a"] = {"best_ms": a_best, "median_ms": a_med, "output": a_out}

        py_out = ""
        if py:
            py_best, py_med, py_out = time_cmd([py, os.path.join(ROOT, cfg["py"])], args.runs)
            row["python"] = {"best_ms": py_best, "median_ms": py_med, "output": py_out}

        cpp_out = ""
        if cpp_bin:
            cpp_best, cpp_med, cpp_out = time_cmd([cpp_bin, cfg["cpp"], "1"], args.runs)
            row["cpp"] = {"best_ms": cpp_best, "median_ms": cpp_med, "output": cpp_out}

        results[name] = row

        line = f"{name:26s} {a_best:9.2f}"
        if py:
            line += f" {row['python']['best_ms']:10.2f}"
        if cpp_bin:
            line += f" {row['cpp']['best_ms']:8.2f}"
            line += f" {a_best / row['cpp']['best_ms']:8.1f}x"
        line += f"  {a_out}"
        print(line)

    # Startup baselines.
    startup = {}
    with tempfile.NamedTemporaryFile("w", suffix=".a", delete=False) as fh:
        fh.write("fn main() -> void:\n    print(0)\n\nmain()\n")
        noop = fh.name
    try:
        startup["a_ms"] = time_cmd([args.a, "run", noop], args.runs)[0]
    finally:
        os.unlink(noop)
    if py:
        startup["python_ms"] = time_cmd([py, "-c", "pass"], args.runs)[0]
    if cpp_bin:
        startup["cpp_ms"] = time_cmd([cpp_bin, "arithmetic", "0"], args.runs)[0]
    print()
    print(
        "startup: "
        + "  ".join(f"{k.replace('_ms', '')} {v:.2f} ms" for k, v in startup.items())
    )

    if args.json:
        payload = {"runs": args.runs, "benchmarks": results, "startup": startup}
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2)
        print(f"wrote {args.json}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
