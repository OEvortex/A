# Memory benchmarks

Allocation-heavy string workload: 500 rounds of converting 200 integers to
strings, joining them with commas, and measuring the result. Stresses
allocation throughput, string formatting, and join behavior.

- A: `strings.a`
- Python: `../equivalents/python/strings.py`
- C++: `../equivalents/cpp/bench.cpp` (mode `memory`)

This is the closest gap to C++ because the C++ side is also allocation-bound
rather than dispatch-bound. Run it with
`RUNS=5 tools/benchmark-runner/run.sh build/a` or compare all three languages
with `python3 tools/benchmark-runner/compare.py`.
