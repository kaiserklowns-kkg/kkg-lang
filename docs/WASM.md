# WASM

Klang compiles to WASM and runs there correctly. This file records how, what was
measured, and what is still missing before a browser page can be written in Klang.

## Building

```sh
klangc program.kkg -o program.c
emcc -O2 program.c -o program.js -sALLOW_MEMORY_GROWTH
node program.js
```

Every example in [examples/](../examples/) passes under Node except two, and both
for reasons that have nothing to do with WASM:

| | |
|---|---|
| `server.kkg` | POSIX sockets; a browser has no `socket(2)` |
| `phase10.kkg` | `spawn`, which needs `-pthread`, `SharedArrayBuffer` and cross-origin isolation |

That includes [tests/gc_stress.kkg](../tests/gc_stress.kkg) and the 120k-allocation
[examples/gc.kkg](../examples/gc.kkg).

## What had to change

The collector. It was **conservative**: it found live objects by scanning the machine
stack and the callee-saved registers for anything that looked like a pointer. That
works natively because C locals live on a stack in addressable memory and `setjmp`
spills the registers onto it.

WASM has no such stack. Locals live in the WASM value stack and in indexed locals,
neither of which is part of linear memory and neither of which can be read by pointer
arithmetic. Only variables the compiler decides to spill reach the shadow stack. A
live pointer held in a WASM local was therefore invisible to the collector, which
freed the object it pointed at.

That was measured, not assumed. The GC regression test built for WASM:

| | after fill | after 120k allocations | after collect |
|---|---|---|---|
| native | `len=500` | `len=500` | `len=500`, contents intact |
| wasm (before) | `len=500` | `len=54104512081969` | `len=0` |
| wasm (now) | `len=500` | `len=500` | `len=500`, contents intact |

The collector is now **precise**: generated code declares its roots and the collector
walks them. See [The collector](LANGUAGE_SPEC.md#the-collector) for the design. Two
things were needed beyond rooting named locals:

- **Every intermediate counts.** In `push(keep, makeNode(i))` the new node is live
  only as an argument. Every GC-typed value now passes through a rooted slot.
- **Allocation must not collect.** A runtime helper allocates an array header and then
  its buffer; between the two the header is only in a C local, in no frame.
  Collecting inside the second allocation freed it. Collection moved to safepoints at
  statement boundaries, where nothing is half-built.

Two build flags make the root set testable on a machine where a failure is debuggable:
`-DK_PRECISE_ONLY=1` turns the stack scan off, reproducing WASM's situation, and
`-DK_GC_STRESS=1` collects at every single allocation so a root missed for one window
is caught rather than surviving by luck. `make test-gc` runs both. They are how the
two bugs above were found.

One 32-bit bug turned up on the way and is fixed: the collector's hash mixed a
`uintptr_t`, which is 32 bits on wasm32, so `x >> 33` was undefined.

## Cost

Rooting is within noise. The 120k-allocation regression test:

| | |
|---|---|
| conservative | 104 ms |
| precise | 107 ms |

## Still missing for frontend

A WASM binary is not yet a web page:

- **DOM access**, which means importing and exporting JavaScript functions — a
  language feature, not a library. This is the next piece of work.
- **Threads** need `-pthread` and, in a browser, `SharedArrayBuffer` with cross-origin
  isolation. `spawn` would map onto Web Workers or be unavailable.
- `std/net` and `std/fs` cannot work in a browser at all; they are POSIX.
