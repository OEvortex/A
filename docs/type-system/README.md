# Type system

A is statically typed with local inference:

- Function parameters and return types can be inferred.
- Bindings may carry annotations (`x: i32 = 1`).
- Numeric operations require matching types; conversions are explicit.
- Optionals are written `T?` and unwrapped with `??` or `!`.
- Function types are written `fn(T, U) -> R`.
- Enums are `i32`-backed. An enum-typed binding (`c: Color`) is compatible
  with variant values and ordinary integer operations; the enum name itself is
  a namespace used for member access.
- Modules have their own type. `m.fn` has a function type, `m.Struct(...)`
  constructs a qualified struct, and `m.value` has the binding's type.
- Constants declared with `const` are immutable but otherwise typed like
  variables.

Struct names in modules are qualified internally (`module::Name`), which is
what lets two modules define a struct with the same short name; diagnostics and
printing use the short name.

The type checker lives in `compiler/source/type-checker`. Full reference:
[docs/language-spec](../language-spec/README.md).
