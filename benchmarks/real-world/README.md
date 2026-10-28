# Real-world benchmarks

Reserved for end-to-end workloads that mimic production programs rather than
isolated kernels. Planned entries:

- a tokenizer plus frequency counter over a synthetic source file
- a struct-heavy simulation loop
- a JSON-like parser and serializer
- a small build-graph resolver

Each entry should exist in A, Python, and C++ under `../equivalents/` before it
is added to the comparison runner, so results are reproducible and equivalent.
