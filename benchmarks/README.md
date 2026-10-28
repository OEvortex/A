# A benchmarks

Performance suite for the reference toolchain. Each category has an A program
and matching C++ / Python equivalents used for cross-language comparison.

| Suite | Workload | A source |
| --- | --- | --- |
| `arithmetic` | 200,000 integer additions in a range loop | `arithmetic/sum.a` |
| `collections` | 5,000-element comprehension + `filter`/`map`/`sum` pipeline | `collections/pipeline.a` |
| `memory` | 500 × 200 `str()` conversions joined into strings | `memory/strings.a` |
| `numerical` | 50,000 `sqrt` calls accumulated as `f64` | `numerical/sqrt.a` |
| `real-world` | Reserved for realistic end-to-end workloads | — |

## Running

```sh
# A only, best of 5
RUNS=5 tools/benchmark-runner/run.sh build/a

# A vs Python vs C++ (compiles the C++ equivalent with g++ -O2)
python3 tools/benchmark-runner/compare.py --runs 5

# Save machine-readable results
python3 tools/benchmark-runner/compare.py --runs 5 --json results.json
```

Equivalents live in [`equivalents/`](equivalents): one C++ translation unit and
one Python file per suite. They implement the same algorithm as the A source;
the C++ bounds are `volatile` so the optimizer cannot fold the benchmark away.

## Baseline and targets

The v0.3 baseline, methodology, and the roadmap to C++ parity are documented in
[`docs/performance/README.md`](../docs/performance/README.md). Only native
`a build` binaries are eligible for the parity gates; `a run` measurements
describe the interpreter and VM.

Reports must record CPU, OS, compiler versions, flags, and run counts. Publish
regressions as well as improvements.
