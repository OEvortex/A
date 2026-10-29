## What changed

One or two sentences describing the change and its motivation. Link issues
with `Closes #<n>` when applicable.

## Verification

- [ ] `cmake --build build -j` succeeds with no new warnings
- [ ] `ctest --test-dir build --output-on-failure` passes
- [ ] `tests/integration/run_tests.sh build/a` passes
- [ ] Behaviour or documented commands changed: `README.md` and `docs/` updated

## Notes for the reviewer

Anything you would like looked at specifically, known limitations, or measured
numbers for performance-sensitive changes.
