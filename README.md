# Klang

Klang is a from-scratch language project: our own syntax, aiming to cover web, app,
AI, backend, ops, and cloud work with one language. The design takes Go's ease of
writing and Rust's safety mindset as inspiration — **not their code, and not their
memory model.** See [docs/LANGUAGE_SPEC.md](docs/LANGUAGE_SPEC.md) for the full
reasoning behind that choice.

The guiding constraint: **easy to write, easy to read, and not much to write.**
Safety should cost you keystrokes, not add them.

## Status: v0.24 — Phase 23 complete

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
- **String interpolation** — `"${name} v${version}"`, no `toString` needed.
- **`mut` parameters**, so a function can say it modifies what it was handed.
- **Our own garbage collector** — precise mark-sweep, written for Klang rather than
  borrowed. Generated code hands over its roots, so the collector never has to guess
  at them; collection happens at safepoints between statements, never inside an
  allocation. A benchmark that peaked at **126 MB** while leaking now peaks at
  **18 MB**, and precision costs 104 ms → 107 ms on the stress test. See
  [The collector](docs/LANGUAGE_SPEC.md#the-collector) for the design and its tradeoffs.
- **`assert(cond, msg)`**, plus `gcCollect()` and `gcHeap()` to inspect the heap.
- **Modules** — `import "std/math"`, with `pub` marking what crosses the boundary and
  everything else private by default. Import paths resolve from the project root, so a
  path always names one module; cycles are reported, not followed.
- **Maps `{K: V}`** — literals, insert-on-assign, `has` / `remove` / `keys` / `values`,
  and `get` returning `Option<V>` when a key may be absent. One open-addressed hash map
  is generated per key/value pair, so there is no boxing.
- **Text** — four string primitives in the compiler, and everything else
  (`split`, `join`, `trim`, `replace`, case, padding, `parseInt` → `Result`) written in
  Klang in [std/string](std/string.kkg).
- **Closures** — `fn(A) -> R` types, `|a, b| ...` literals, parameter types inferred
  from context. Capturing works exactly like passing an argument, environments are
  GC-allocated so a closure can outlive its maker, and closures nest, sort, and live
  in arrays and maps.
- **Left-to-right evaluation**, guaranteed where C leaves the order unspecified.
- **Method calls** — `x.f(a)` is `f(x, a)`, so every function is usable both ways and
  chains read in the order the work happens.
- **`const`**, **`break` / `continue`**, **`+= -= *= /= %=`**, nesting `/* */` comments,
  and **camelCase throughout** — the syntax is settled, not still moving.
- **Calling C** — `extern fn`, `extern type` handles, `extern header` / `extern link`.
  Declaring is free; **calling is `unsafe`**, so `grep unsafe` finds every place the
  compiler stopped vouching for you. [std/fs](std/fs.kkg) does the unsafe work once
  and exports a fully safe `Result` API — using it needs no `unsafe` at all.
- **Checked arithmetic** — integer overflow and division by zero stop with a message
  instead of wrapping or raising a signal. Signed overflow is undefined behaviour in
  C, so this is not pedantry. Measured at 5–15% on a loop of pure arithmetic;
  `wrapAdd` / `wrapSub` / `wrapMul` when wrapping is what you actually mean.
- **`toInt` / `toFloat`** — no implicit numeric conversions, and no undefined edges.
- **Recursive types** — a type may contain itself through an array or map, because
  those are references. Trees, syntax trees and JSON documents are expressible;
  by-value self-containment is still an error, with the fix in the message.
- **[std/json](std/json.kkg)** — parse and stringify, written in Klang with no FFI.
  Errors carry the byte offset of the problem.
- **Backend** — [std/net](std/net.kkg) gives TCP over POSIX sockets behind a `Result`
  API, [std/http](std/http.kkg) parses requests and builds responses, and
  [examples/server.kkg](examples/server.kkg) is a working HTTP server: HTML, JSON,
  query strings, POST bodies, 404s. It proves itself — a client task runs on another
  thread while the server accepts, so `make check` covers it without needing curl.
- **A standard library has started**: [std/math](std/math.kkg),
  [std/list](std/list.kkg) (map/filter/reduce/find/sorted/…),
  [std/string](std/string.kkg), [std/fs](std/fs.kkg), [std/net](std/net.kkg),
  [std/http](std/http.kkg), [std/json](std/json.kkg), [std/ui](std/ui.kkg),
  [std/dom](std/dom.kkg), [std/fetch](std/fetch.kkg).
- **WASM** — Klang compiles to WASM and runs there correctly. Every example passes
  under Node except the two that need POSIX sockets or threads. This is what the
  precise collector was for: WASM locals are not addressable memory, so the old
  conservative scan found nothing there and freed live objects. See
  [docs/WASM.md](docs/WASM.md) for the before/after measurements.
- **`if` as an expression** — `let label = if n < 0 { "neg" } else { "pos" }`. Braces
  hold one expression and `else` is required, so braces keep meaning one thing.
- **You write Klang and nothing else** — no HTML, no CSS, no JavaScript, no C.
  A web project is one `.kkg` file: [std/ui](std/ui.kkg) is React's model in
  Klang — a component is a function returning a `Node`, handlers are closures,
  and `refresh` diffs the DOM rather than rebuilding it, so a live `<input>`
  keeps what is being typed into it. Escaping is structural.
- **Styling is Tailwind** — one way, not three. `klangc new` scaffolds it and
  `klangc web run` compiles it, with `@source` pointed at `src/**/*.kkg` so the
  class names Klang writes into string literals are found. That is checked by
  compiling Tailwind for real in `make check`, not asserted.
- **Frontend plumbing** — `js fn` declares a function whose body is JavaScript and
  lets the compiler marshal the boundary; `export fn` makes a Klang function
  callable from JavaScript. [std/dom](std/dom.kkg) covers text, attributes,
  classes, storage, the address bar and timers; [std/fetch](std/fetch.kkg) is
  HTTP, taking closures and requiring an error handler on every call.
  [examples/web.kkg](examples/web.kkg) is a working application — a form, a keyed
  list, routing, state persisted as JSON, and a sync that posts it — written
  entirely in Klang, and `make test-web` drives all of it against a stub DOM and
  a stub server.
- **Full stack, tested as one** — [examples/fullstack](examples/fullstack) is a
  REST API as a native binary and a page as WebAssembly, both Klang.
  `make test-fullstack` drives the page against the running server over real
  HTTP, with nothing stubbed between them.
- **Native executables** — `klangc build` produces a binary the OS runs, linked
  against nothing but the C library. `--static` needs not even that; `--debug`
  keeps symbols. C is the intermediate form, kept in `.klang/` to be read rather
  than handled. A program with no `js fn` has no JavaScript-boundary code emitted
  into it at all, and `make check` asserts it.
- **A toolchain, not just a compiler** — `klangc new` writes a project that
  already runs (web, cli or server); `klangc run` builds and executes; `klangc web
  run` does the browser equivalent — emcc, a dev server, and a browser, in one
  command. It serves with **bun** if it is installed and node otherwise; both are
  supported and `make test-web` runs the example under each. The dev server is
  generated rather than depended on, and `make check` scaffolds all three project
  kinds and runs them.

Still missing, in the order it is likely to bite: **traits** (generics are
unbounded, so a generic function can only do what works for every type), a
**package manager** (`import` reaches the standard library and your own files, and
no further), **command-line arguments**, a **native clock and RNG**, **threads on
the web** (they need `SharedArrayBuffer` and cross-origin isolation — native
threads work), and **Unicode beyond bytes**. `std/net` is POSIX only; Windows
needs the Winsock variant. No REPL, formatter, linter or language server.
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
// Closures. Types are inferred from what the function expects.
let adults = list.filter(people, |p| p.age >= 18)
let byName = list.sorted(adults, |a, b| a.name < b.name)
let line   = list.join(list.map(byName, |p| p.name), ", ")
```

```kkg
// Calling C: declaring is free, calling is unsafe, and a safe wrapper
// discharges the obligation once so callers never see it.
extern fn cSqrt(x: float) -> float = "sqrt"

fn squareRoot(x: float) -> Result<float, string> {
    if x < 0.0 { return Err("no real square root of ${x}") }
    return Ok(unsafe { cSqrt(x) })
}
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

```kkg
// The browser. `js fn` bodies are JavaScript; the compiler marshals the boundary,
// std/dom wraps it safely, and `export fn` is what an event handler can name.
import "std/dom"

let mut clicks = 0

export fn onClick() {
    clicks += 1
    dom.setText("#count", "${clicks} clicks")
}

fn main() {
    dom.on("#button", "click", "onClick")
}
```

See [examples/web.kkg](examples/web.kkg),
[examples/phase1.kkg](examples/phase1.kkg),
[examples/phase2.kkg](examples/phase2.kkg),
[examples/modules.kkg](examples/modules.kkg),
[examples/phase5.kkg](examples/phase5.kkg),
[examples/phase6.kkg](examples/phase6.kkg),
[examples/phase7.kkg](examples/phase7.kkg),
[examples/phase8.kkg](examples/phase8.kkg),
[examples/phase9.kkg](examples/phase9.kkg),
[examples/phase10.kkg](examples/phase10.kkg),
[examples/server.kkg](examples/server.kkg),
[examples/json.kkg](examples/json.kkg), and
[examples/gc.kkg](examples/gc.kkg) for the full tour.

## Build

Requires a C99 compiler (`gcc` or `clang`).

```sh
make            # builds bin/klangc
make check      # runs the examples and the compile-error tests
make test-web   # builds the browser example and drives it headlessly
```

`make check` needs only a C compiler. `make test-web` also wants
[Emscripten](https://emscripten.org) and [bun](https://bun.sh) or node, and
skips itself politely when they are absent.

## Try it

Klang is a compiled language. `build` produces an executable the operating system
runs — no runtime to ship, no interpreter, no JavaScript anywhere near it:

```sh
bin/klangc new myapp --kind cli
cd myapp
klangc build src/main.kkg      # -> ./main, 41 KB
./main
```

```
$ ldd main
    libm.so.6 => /lib/x86_64-linux-gnu/libm.so.6
    libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6
```

That is the whole dependency list. `--static` links even those in, so the binary
runs on a machine with nothing installed. `--debug` skips optimization and keeps
symbols. `klangc run` builds and executes in one step.

C is the intermediate form, the way assembly is for a traditional compiler; it is
kept in `.klang/` to be read, not handled. A program with no `js fn` has no
JavaScript-boundary code emitted into it at all — `make check` asserts that.

For the browser, the same source goes the other way:

```sh
bin/klangc new mypage          # --kind web is the default
cd mypage
klangc web run                 # emcc, a dev server, a browser
```

`new` writes a working program, not a folder of empty files:

```
myapp/
  src/main.kkg      the program
  web/index.html    the page          (--kind web)
  web/style.css     or input.css, with --css tailwind
  README.md         how to run this one
  .gitignore
```

`--kind cli`, `--kind server` and `--kind web` are all built and executed by
`make check`. Or point the compiler at a file directly:

```sh
bin/klangc build examples/phase2.kkg
bin/klangc run examples/server.kkg
bin/klangc web run examples/web.kkg
```

## License

Apache License 2.0 — see [LICENSE](LICENSE).
