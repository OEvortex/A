# Memory model

A is a memory-safe language: there are no pointers, no manual allocation, and
no destructors to write.

- Integers, floats, booleans, and characters are value types.
- Strings are immutable and encoded as UTF-8.
- Arrays, structs, tuples, optionals, and closures are reference values with
  automatic lifetime management (shared ownership in the interpreter).
- Assignment copies the reference for reference values and the value for scalar
  values.

## Environments and closures

Every module owns a global environment for its top-level bindings. Functions
and lambdas resolve globals through the environment of the module where they
were defined, so a function exported from a module keeps reading that module's
state even when called elsewhere.

Lambdas capture visible local bindings **by value** at creation time. Captured
arrays and structs are references, so mutations through them are visible, but
rebinding a captured scalar afterwards is not. Module globals are not copied;
they are read live at call time.

## Modules

Imported modules execute once and their environments live for the whole
program. `m.name` reads the module's current binding and `m.name = value`
writes it, so module-level `var` state is shared, while `const` declarations
and `from ... import` bindings cannot be reassigned.

Memory is reclaimed automatically when the last reference to an array, struct,
tuple, optional, or closure is dropped. The tree-walking interpreter uses
shared ownership; a future native backend will keep the same observable
semantics.

This document will be expanded as the native backend lands.
