# Klang Language Spec (v0.3)

> This spec describes the language we are building **from scratch**. Nothing here is
> retrofitted from a previous implementation — this document is the source of truth,
> and the compiler in [src/](../src/) is being built to match it, one feature at a time.
> Anything not listed under "Implemented today" below does not exist in the compiler yet.

## Design decisions (locked in)

- **Memory model: garbage collected.** No borrow checker, no lifetimes, no `move`/`&`/`&mut`.
  This is the single biggest simplification vs. Rust and the main reason Klang aims to be as
  easy to pick up as Go.
- **Safety comes from the type system, not from ownership tracking:**
  - No implicit null. Absence is `Option<T>` (`Some(x)` / `None`).
  - Fallible operations return `Result<T, E>` (`Ok(x)` / `Err(e)`), propagated with `?`.
  - `match` on an enum (including `Option`/`Result`) must cover every case, or the compiler
    rejects it — no silently-forgotten branches.
  - Variables are immutable by default (`let`); `let mut` is required to reassign.
- **Ownership is enforced in exactly one place:** values passed across a thread/channel
  boundary can't be used from the sending side afterward. This is the one data-race guard
  kept from Rust's model; everywhere else, ownership is not tracked.
- **Generics are real**, not just syntax: the compiler monomorphizes — each instantiation of
  a generic function/struct with a concrete type gets its own generated code.
- **Backend: transpile to C.** `.kkg` → C → whatever C compiler is on the host. This is what
  makes "every platform" achievable without writing N native backends up front.

## Syntax tour

### Variables

```kkg
let name = "Klang"       // immutable, type inferred
let mut count = 0        // mutable
count = count + 1        // ok, count is mut
// name = "x"             // error: cannot assign to immutable variable

let score: int = 100     // explicit type annotation
```

### Functions

```kkg
fn add(a: int, b: int) -> int {
    return a + b
}

fn greet(name: string) {   // no return type = returns nothing
    println("Hello, " + name)
}
```

### Control flow

```kkg
if x > 0 {
    println("positive")
} else if x < 0 {
    println("negative")
} else {
    println("zero")
}

let mut i = 0
while i < 10 {
    println("${i}")
    i = i + 1
}
```

### Structs

```kkg
struct Point {
    x: int,
    y: int,
}

let p = Point { x: 1, y: 2 }
println("${p.x}, ${p.y}")
```

### Enums, Option, Result, match

```kkg
enum Shape {
    Circle(float),
    Rect(float, float),
}

fn area(s: Shape) -> float {
    match s {
        Circle(r)     => 3.14159 * r * r,
        Rect(w, h)    => w * h,
    }
}

fn find_user(id: int) -> Option<User> {
    if !db.has(id) { return None }
    return Some(db.get(id))
}

fn read_config(path: string) -> Result<Config, string> {
    let text = fs.read(path)?     // propagate error early
    return parse_config(text)
}
```

### Arrays

`[T]` is a growable array of `T`. Arrays are **references to a heap object**: passing one
to a function, or binding it to another name, does not copy it — pushing through either
name is visible through both. Indexing is bounds-checked at runtime; going out of range
aborts with the index and the length rather than reading garbage.

```kkg
let nums = [3, 1, 4]
println("${len(nums)} items, first is ${nums[0]}")

let mut xs: [int] = []     // an empty literal needs to know what it holds
push(xs, 10)
xs[0] = 99

let grid = [[1, 2], [3, 4]]   // arrays nest
```

Mutation follows the same rule as everything else: `let` is enough to read an array, but
pushing to it or assigning into it requires `let mut`.

### Loops

`for ... in` walks an array or an `a..b` range (`a` inclusive, `b` exclusive). The
iterable is evaluated exactly once. The loop variable is rebound each turn, so it is
never `mut`.

```kkg
for n in nums        { println("${n}") }
for i in 0..len(nums) { println("${i}: ${nums[i]}") }
```

### String interpolation

`${...}` takes any expression. Values are converted for you, so the common case needs no
`to_string` call. Write `\${` for a literal dollar-brace.

```kkg
println("${name} v${version} — ${2 + 2} items")
```

### Mutable parameters

A parameter marked `mut` says the function may modify it. For an array — a reference —
that modification is visible to the caller. For a value type like `int` or a struct, the
function is mutating its own copy, exactly as in Go.

```kkg
fn restock(mut items: [Item], name: string) {
    push(items, Item { name: name, qty: 1 })   // the caller sees this
}
```

### Generics

Generic functions, structs, and enums are monomorphized: every concrete instantiation
becomes its own generated function/type, so there is no boxing and no runtime cost.

```kkg
fn first_or<T>(value: Option<T>, fallback: T) -> T {
    return match value {
        Some(v) => v,
        None    => fallback,
    }
}

struct Pair<A, B> {
    left: A,
    right: B,
}

let n = first_or(Some(7), 0)            // T = int
let s = first_or(None, "default")       // T = string, inferred from the 2nd argument
let p = Pair { left: 1, right: "one" }  // Pair<int, string>
```

### Concurrency (target)

```kkg
let data = [1, 2, 3]
let ch = channel()
spawn {
    ch.send(data)      // data's ownership moves into the channel
}
// data is no longer usable here — compiler error if you try
let received = ch.receive()
```

## Implemented today (v0.3, Phase 2 complete)

**v0.1 core**
- `let`, `let mut`, immutability enforcement
- Types: `int`, `float`, `bool`, `string`
- `fn` with typed params, typed or omitted return type
- `if` / `else if` / `else`, `while`
- `struct` definitions, struct literals, field access
- Arithmetic (`+ - * / %`), comparison (`== != < <= > >=`), logical (`&& || !`), unary `-`
- String `+` concatenation and `==` / `!=` comparison
- `return`; builtins `println`, `print`, `to_string`

**Phase 1 additions**
- `enum` with payload-carrying variants
- `match`, as an expression *and* as a statement; arms take `=> expr` or `=> { ... }`
- **Exhaustiveness checking** — a `match` missing a variant is a compile error that names
  the missing variants; `_` is the catch-all; duplicate arms are rejected
- Built-in `Option<T>` (`Some`/`None`) and `Result<T, E>` (`Ok`/`Err`) — no import needed
- `?` operator to propagate `Err`/`None` early, with error-type checking
- Real generics for functions, structs, and enums via **monomorphization**
- Type-argument inference that isn't left-to-right: `first_or(None, "default")` infers
  `T = string` from the second argument
- Variants are written unqualified (`Some(x)`, not `Option::Some(x)`)

**Phase 2 additions**
- `[T]` arrays: literals, indexing, `a[i] = v`, nesting, and generics over them
- Bounds-checked indexing — out of range aborts with the index and the length
- `len(...)` on arrays and strings; `push(...)` to append
- `for x in array` and `for i in a..b`, with the iterable evaluated exactly once
- String interpolation: `"${expr}"`, converting values automatically; `\${` escapes it
- `mut` parameters, so a function can declare that it modifies what it was given
- Mutation rules extend to arrays: pushing or index-assigning needs `let mut`

Generated C compiles clean under `-Wall -Wextra`.

## Not yet implemented

- Maps, closures, traits, modules/imports
- Recursive types (they need indirection, which the language does not have yet — the
  compiler rejects them with a clear message rather than looping)
- `to_string` / `==` on structs and enums (match on them instead)
- Thread/channel send-ownership checking, `spawn`/`await`
- Any backend other than C (LLVM, WASM are not planned near-term — C gets us "every platform"
  for free via the host's C toolchain)
- Tooling: REPL, formatter, linter, package manager — all future work, not started
