# Benchmark runner

`run.sh` times every program under `benchmarks/*/*.a` with the A interpreter
and prints the best wall-clock time plus the program output.

```sh
tools/benchmark-runner/run.sh build/a
RUNS=5 tools/benchmark-runner/run.sh build/a
```

`RUNS` controls repetitions (default 3). The runner exits non-zero if the
binary is missing or a benchmark fails.
