# Numerical benchmarks

Floating-point math throughput: 50,000 `sqrt` calls accumulated into an `f64`,
then rounded. Exercises floating-point dispatch and the native math bindings.

- A: `sqrt.a`
- Python: `../equivalents/python/sqrt.py`
- C++: `../equivalents/cpp/bench.cpp` (mode `numerical`)

Run it with `RUNS=5 tools/benchmark-runner/run.sh build/a` or compare all three
languages with `python3 tools/benchmark-runner/compare.py`.
