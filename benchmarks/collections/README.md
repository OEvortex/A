# Collection benchmarks

A 5,000-element list comprehension fed through a `filter` → `map` → `sum`
pipeline. Exercises array allocation, lambda creation and calls, iteration, and
the pipeline operators.

- A: `pipeline.a`
- Python: `../equivalents/python/pipeline.py`
- C++: `../equivalents/cpp/bench.cpp` (mode `collections`)

Run it with `RUNS=5 tools/benchmark-runner/run.sh build/a` or compare all three
languages with `python3 tools/benchmark-runner/compare.py`.
