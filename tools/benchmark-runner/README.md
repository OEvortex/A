# Benchmark runner

`run.sh` times every program under `benchmarks/*/*.a` with the A interpreter
and prints the best wall-clock time plus the program output.

```sh
tools/benchmark-runner/run.sh build/a
RUNS=5 tools/benchmark-runner/run.sh build/a
```

`RUNS` controls repetitions (default 3). The runner exits non-zero if the
binary is missing or a benchmark fails.

## Cross-language comparison

`compare.py` times the same workloads in A, Python, and C++ and prints a table
with the A/C++ ratio. The C++ equivalent is compiled once with `g++ -O2` (or
`clang++` if g++ is unavailable) into a temporary directory.

```sh
python3 tools/benchmark-runner/compare.py --runs 5
python3 tools/benchmark-runner/compare.py --runs 7 --json results.json
python3 tools/benchmark-runner/compare.py --skip-cpp   # no C++ compiler
```

It reports best-of-N wall time (including process startup) plus a separate
startup baseline for each language, and can write JSON for CI artifacts.
Measurements are end-to-end; compute-only numbers and parity targets are in
[`docs/performance`](../../docs/performance/README.md).

