# WASM: where it stands

Short version: **the code generator targets WASM correctly, and the garbage
collector does not.** Klang cannot yet be used for frontend work, and this file
records exactly why, what was measured, and what has to change.

## What already works

Klang compiles to C, and that C compiles to WASM and runs:

```sh
klangc program.kkg -o program.c
emcc -O2 program.c -o program.js
node program.js
```

A program exercising closures, generics, maps, string interpolation and the
standard library produced the right answers under Node:

```
hello from wasm
sorted = 2, 4, 10, 16, 18
upper  = KLANG
a appears 3 times
```

So the language, the type system, monomorphization and the runtime helpers are
all fine on a 32-bit target. One genuine bug turned up on the way and is fixed:
the collector's hash mixed a `uintptr_t`, which is 32 bits on wasm32, so
`x >> 33` was undefined and the mixing collapsed.

## What does not work

The collector is **conservative**: it finds live objects by scanning the machine
stack and registers for anything that looks like a pointer. That works natively
because C locals live on a stack in addressable memory, and `setjmp` spills the
registers onto it.

WASM has no such stack. Locals live in the WASM value stack and in indexed
locals, neither of which is part of linear memory and neither of which can be
read by pointer arithmetic. Only variables the compiler decides to spill end up
in the shadow stack. A live pointer held in a WASM local is therefore invisible
to the collector, which frees the object it points at.

This is not theoretical. Building the existing GC regression test for WASM:

| | after fill | after 120k allocations | after collect |
|---|---|---|---|
| native | `len=500` | `len=500` | `len=500`, contents intact |
| wasm | `len=500` | `len=54104512081969` | `len=0` |

The array of 500 live records is freed while it is still in use, and its header
reads back as garbage. Disabling collection makes the WASM run correct again
(`len=500` throughout), which confirms the collector is the cause rather than
codegen or the 32-bit target.

## What would fix it

The collector has to become **precise**: instead of guessing at roots by scanning
memory, generated code must hand the collector its roots explicitly. Concretely, a
frame of pointers per function, linked into a list the collector walks, maintained
so it is correct at every exit including the early returns that `?` produces.

That was considered and deliberately rejected when the collector was written
(see [The collector](LANGUAGE_SPEC.md#the-collector)): conservative scanning needs
no cooperation from generated code, so every local, temporary and register-resident
value is covered for free, while a precise root set has to be right in far more
places and fails by freeing live data rather than by retaining dead data.

WASM changes that trade-off, because conservative scanning is not merely
approximate there — it does not work at all. Precision is no longer the more
error-prone option; it is the only one.

The work is not only rooting named locals. Every intermediate value counts too:
in `push(keep, makeNode(i))` the new node is live only as an argument while `push`
may allocate and collect. Every GC-typed value would need to pass through a rooted
slot, which the existing operand-pinning machinery could be extended to do.

## Also missing for frontend

Even with a precise collector, browser work needs more than a WASM binary:

- **DOM access**, which means a story for importing and exporting JavaScript
  functions — a language feature, not a library.
- **Threads**, which on the web need `SharedArrayBuffer` and cross-origin
  isolation. `spawn` would have to map onto Web Workers or be unavailable.
- `std/net` and `std/fs` cannot work in a browser at all; they are POSIX.

## Status

Do not describe Klang as supporting WASM. The honest statement is that its output
compiles and runs there, and that anything which allocates enough to trigger a
collection is unsafe until the collector is precise.
