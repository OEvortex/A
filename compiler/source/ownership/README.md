# Ownership analysis

`analyzeOwnership` runs after type checking and reports moves and borrow
violations before any backend executes.

- `move(x)` transfers ownership: later reads of `x` are errors, and `x` may
  not be moved or borrowed while a view of it is live.
- A unit-step slice (`xs[a..b]`) borrows `xs`. While the view is live,
  structural mutators (`append`, `extend`, `insert`, `pop`, `remove`,
  `remove_at`, `clear`) on the lender are rejected. Element writes,
  `reverse`, and `sort` remain allowed, matching runtime behavior.
- Reassigning a binding releases its borrow. `if`/`while`/`for`/`loop` merge
  moved and borrowed state across branches conservatively; `defer` bodies are
  analysed without leaking state into the enclosing block.

Method calls on a view itself are not treated as lender mutations; the runtime
rejects structural mutation of a view directly.
