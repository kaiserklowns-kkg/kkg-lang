# Klang

Klang is a from-scratch language project: our own syntax, aiming to cover web, app,
AI, backend, ops, and cloud work with one language. The design takes Go's ease of
writing and Rust's safety mindset as inspiration — **not their code, and not their
memory model.** See [docs/LANGUAGE_SPEC.md](docs/LANGUAGE_SPEC.md) for the full
reasoning behind that choice.

The guiding constraint: **easy to write, easy to read, and not much to write.**
Safety should cost you keystrokes, not add them.

## Status: v0.2 — Phase 1 complete

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

Not yet: arrays/maps, closures, traits, modules, concurrency, and all tooling
(REPL, formatter, linter, package manager). See the
[spec's status section](docs/LANGUAGE_SPEC.md#not-yet-implemented).

## A taste

```kkg
// Errors are values. `?` propagates them.
fn describe_port(text: string) -> Result<string, string> {
    let port = parse_port(text)?
    return Ok("port is " + to_string(port))
}

// No null. The compiler makes you handle the empty case.
fn safe_div(a: int, b: int) -> Option<int> {
    if b == 0 {
        return None
    }
    return Some(a / b)
}

match safe_div(10, 2) {
    Some(v) => println("10/2 = " + to_string(v)),
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

See [examples/phase1.kkg](examples/phase1.kkg) for the full tour.

## Build

Requires a C99 compiler (`gcc` or `clang`).

```sh
make            # builds bin/klangc
make check      # runs the examples and the compile-error tests
```

## Try it

```sh
bin/klangc examples/phase1.kkg -o out.c
gcc -O2 -o out out.c
./out
```

## License

Apache License 2.0 — see [LICENSE](LICENSE).
