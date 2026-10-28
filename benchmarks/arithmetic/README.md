# Arithmetic benchmarks

Integer throughput in a tight range loop: 200,000 checked integer additions
accumulated into a single binding.

- A: `sum.a`
- Python: `../equivalents/python/sum.py`
- C++: `../equivalents/cpp/bench.cpp` (mode `arithmetic`)

The workload isolates interpreter dispatch, variable lookup, boxed-value
creation, and checked arithmetic. Run it with
`RUNS=5 tools/benchmark-runner/run.sh build/a` or compare all three languages
with `python3 tools/benchmark-runner/compare.py`.
