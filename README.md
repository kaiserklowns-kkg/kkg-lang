# Klang

Klang is a from-scratch language project: our own syntax, aiming to cover web, app,
AI, backend, ops, and cloud work with one language. The design takes Go's ease of
writing and Rust's safety mindset as inspiration — **not their code, and not their
memory model.** See [docs/LANGUAGE_SPEC.md](docs/LANGUAGE_SPEC.md) for the full
reasoning behind that choice.

The guiding constraint: **easy to write, easy to read, and not much to write.**
Safety should cost you keystrokes, not add them.

## Status: v0.6 — Phase 5 complete

Everything listed here is implemented and covered by `make check`. Nothing below is
aspirational.

- Self-contained compiler ([src/klangc.c](src/klangc.c)): lex → parse → typecheck →
  monomorphize → emit C. The generated C builds clean under `-Wall -Wextra`.
- `let` / `let mut` with enforced immutability, `int` / `float` / `bool` / `string`,
  `fn`, `struct`, `if` / `while`, string concat and comparison.
- **Enums with payloads**, and `match` as both expression and statement.
- **Exhaustive matching** — forgetting a variant is a compile error that tells you
  which ones you missed.
- **Built-in `Option<T>` and `Result<T, E>`** — no null, no imports.
- **`?` operator** to propagate errors in one character.
- **Real generics** for functions, structs, and enums, via monomorphization —
  no boxing, no runtime cost.
- **Arrays `[T]`** — literals, indexing, nesting, generics over them, and bounds
  checks that report the index and the length instead of reading garbage.
- **`for x in xs` and `for i in a..b`**, plus `len` and `push`.
- **String interpolation** — `"${name} v${version}"`, no `to_string` needed.
- **`mut` parameters**, so a function can say it modifies what it was handed.
- **Our own garbage collector** — conservative mark-sweep over the machine stack and
  registers, written for Klang rather than borrowed. A benchmark that peaked at
  **126 MB** while leaking now peaks at **18 MB**. See
  [The collector](docs/LANGUAGE_SPEC.md#the-collector) for the design and its tradeoffs.
- **`assert(cond, msg)`**, plus `gc_collect()` and `gc_heap()` to inspect the heap.
- **Modules** — `import "std/math"`, with `pub` marking what crosses the boundary and
  everything else private by default. Import paths resolve from the project root, so a
  path always names one module; cycles are reported, not followed.
- **Maps `{K: V}`** — literals, insert-on-assign, `has` / `remove` / `keys` / `values`,
  and `get` returning `Option<V>` when a key may be absent. One open-addressed hash map
  is generated per key/value pair, so there is no boxing.
- **Text** — four string primitives in the compiler, and everything else
  (`split`, `join`, `trim`, `replace`, case, padding, `parseInt` → `Result`) written in
  Klang in [std/string](std/string.kkg).
- **A standard library has started**: [std/math](std/math.kkg),
  [std/list](std/list.kkg), [std/string](std/string.kkg).

Not yet: closures, traits, concurrency, and all tooling
(REPL, formatter, linter, package manager). Integer overflow still wraps silently.
See the [spec's status section](docs/LANGUAGE_SPEC.md#not-yet-implemented).

## A taste

```kkg
// Errors are values. `?` propagates them. `${}` interpolates anything.
fn describe_port(text: string) -> Result<string, string> {
    let port = parse_port(text)?
    return Ok("port is ${port}")
}

// Arrays, ranges, and for-in. Bounds are checked.
fn evens(upto: int) -> [int] {
    let mut out: [int] = []
    for n in 0..upto {
        if n % 2 == 0 { push(out, n) }
    }
    return out
}

// No null. The compiler makes you handle the empty case.
fn safe_div(a: int, b: int) -> Option<int> {
    if b == 0 {
        return None
    }
    return Some(a / b)
}

match safe_div(10, 2) {
    Some(v) => println("10/2 = ${v}"),
    None    => println("cannot divide"),
}

// Generics, inferred in whichever direction the types actually flow.
fn first_or<T>(value: Option<T>, fallback: T) -> T {
    return match value {
        Some(v) => v,
        None    => fallback,
    }
}

first_or(Some(7), 0)          // 7        — T = int
first_or(None, "default")     // default  — T = string, from the 2nd argument
```

```kkg
// Modules: private by default, `pub` to export. Maps and text.
import "std/string" as str

fn wordCount(text: string) -> {string: int} {
    let mut counts: {string: int} = {}
    for w in str.split(str.toLower(text), " ") {
        if has(counts, w) { counts[w] = counts[w] + 1 } else { counts[w] = 1 }
    }
    return counts
}
```

See [examples/phase1.kkg](examples/phase1.kkg),
[examples/phase2.kkg](examples/phase2.kkg),
[examples/modules.kkg](examples/modules.kkg),
[examples/phase5.kkg](examples/phase5.kkg), and
[examples/gc.kkg](examples/gc.kkg) for the full tour.

## Build

Requires a C99 compiler (`gcc` or `clang`).

```sh
make            # builds bin/klangc
make check      # runs the examples and the compile-error tests
```

## Try it

```sh
bin/klangc examples/phase2.kkg -o out.c
gcc -O2 -o out out.c
./out
```

## License

Apache License 2.0 — see [LICENSE](LICENSE).
