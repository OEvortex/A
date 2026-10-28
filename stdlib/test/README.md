# test

`test.a` is an A-file module that demonstrates stdlib resolution and live
module state:

```a
import test

test.check("addition", 1 + 1 == 2)
test.equal("double", 6, 3 * 2)
test.require(true, "always true")
test.report()               # "2 passed, 0 failed"
```

It exports the counters `passed` and `failed` plus `check`, `equal`, `require`,
`summary` (the failure count), and `report`.
