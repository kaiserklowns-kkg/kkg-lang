# Klang Language Spec (v0.11)

> This spec describes the language we are building **from scratch**. Nothing here is
> retrofitted from a previous implementation — this document is the source of truth,
> and the compiler in [src/](../src/) is being built to match it, one feature at a time.
> Anything not listed under "Implemented today" below does not exist in the compiler yet.

## Design decisions (locked in)

- **Everything is `camelCase`** — builtins, standard library, your own code. One rule,
  no guessing which side of the fence a name is on. This was settled before anyone
  depended on the names, because renaming later breaks every line already written.
- **`x.f(a)` means exactly `f(x, a)`.** Method syntax is a way of writing a call, not a
  separate kind of declaration, so every function is usable both ways and the standard
  library needed no rewriting to gain it.

- **Memory model: garbage collected.** No borrow checker, no lifetimes, no `move`/`&`/`&mut`.
  This is the single biggest simplification vs. Rust and the main reason Klang aims to be as
  easy to pick up as Go. The collector is Klang's own — see [The collector](#the-collector).
- **Safety comes from the type system, not from ownership tracking:**
  - No implicit null. Absence is `Option<T>` (`Some(x)` / `None`).
  - Fallible operations return `Result<T, E>` (`Ok(x)` / `Err(e)`), propagated with `?`.
  - `match` on an enum (including `Option`/`Result`) must cover every case, or the compiler
    rejects it — no silently-forgotten branches.
  - Variables are immutable by default (`let`); `let mut` is required to reassign.
- **Nothing mutable is ever shared between threads.** This turned out to be a stronger
  guarantee than the move rule originally planned, and a simpler one: immutable values
  cross for free, mutable ones are copied, so two threads never hold the same object.
  A data race is not something to be careful about — it cannot be written. See
  [Concurrency](#concurrency).
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

### Maps

`{K: V}` is a hash map. Like arrays it is a reference to a heap object, and the same
mutation rule applies: reading needs only `let`, inserting or removing needs `let mut`.
Keys must be `int`, `string` or `bool` — anything else is a compile error rather than a
surprise at runtime.

```kkg
let ages = {"ada": 36, "alan": 41}
let mut stock: {string: int} = {}      // an empty literal needs to know what it holds
stock["apple"] = 3                     // inserts
stock["apple"] = stock["apple"] + 1    // updates
remove(stock, "apple")

len(m)  has(m, k)  keys(m)  values(m)
```

Reading a missing key aborts, the same way an out-of-range array index does. When
absence is a normal outcome, ask for it as a value instead:

```kkg
match get(ages, "bob") {
    Some(a) => println("bob is ${a}"),
    None    => println("no bob"),
}
```

`keys` and `values` hand back arrays, so iterating is just the ordinary `for`:

```kkg
for name in keys(ages) { println("${name} is ${ages[name]}") }
```

### Text

The compiler knows four string primitives; everything else lives in
[std/string](../std/string.kkg), written in Klang on top of them. That is deliberate —
the standard library should not be a privileged place with powers ordinary code lacks.

```kkg
substr(s, start, end)   // byte offsets, clamped — never reads out of range
byteAt(s, i)           // bounds-checked byte value
fromByte(code)         // one-byte string
indexOf(s, needle)     // byte offset, or -1
```

Built on those, `std/string` provides `split`, `join`, `replace`, `trim`, `toUpper`,
`toLower`, `startsWith`, `endsWith`, `contains`, `charAt`, `repeat`, `reverse`,
`padStart`, `padEnd`, and `parseInt` — which returns `Result<int, string>`, so bad input
is a value you handle rather than a crash.

Offsets are byte offsets. ASCII is exact; other UTF-8 passes through unchanged as long
as you slice at boundaries found with `indexOf`.

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
`toString` call. Write `\${` for a literal dollar-brace.

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

### Method calls

`x.f(a)` is `f(x, a)`. There is no separate method declaration: any function whose
first parameter accepts the receiver can be called this way, including builtins and
anything in an imported module.

```kkg
fn grade(p: Player) -> string { ... }

p.grade()                       // same call as grade(p)
p.withBonus(10).grade()         // chains read in the order work happens

"  a, b ".trim().split(",").map(|s| s.trim()).sorted(|a, b| a < b)
xs.len()   m.has(k)   text.toUpper()
```

Resolution is deterministic: the current module, then the prelude, then each imported
module — taking only `pub` functions from elsewhere. Exactly one candidate must accept
the receiver as its first argument; if two do, the compiler names both and asks you to
call one directly. A struct field holding a closure takes precedence over a free
function of the same name, since it really is a member.

### Constants

```kkg
const MAX: int = 100
const APP = "klang"
const BANNER = "── ${APP} ──"        // may use constants written above it
const VOWELS = ["a", "e", "i", "o", "u"]
```

`const` is module-level and follows the same `pub` rule as everything else. An
initializer is an ordinary expression evaluated once at startup, so it may allocate.
Constants are set up in the order they are written, and using one before it is defined
is an error rather than a silent zero.

### Loops, breaking, and compound assignment

```kkg
for n in xs {
    if n < 0 { continue }
    if n > 100 { break }
    total += n
}
```

`break` and `continue` work in `while` and `for`, including from inside a `match` — the
match does not swallow them. They refer to the nearest enclosing loop *in the same
function*, so a loop outside a closure is not breakable from within it.

`+=`, `-=`, `*=`, `/=` and `%=` expand to `x = x <op> v`. The target is evaluated
twice, so keep calls out of the index of a compound assignment.

### Comments

`// to end of line`, and `/* ... */` which **nests**, so commenting out a region that
already contains a block comment does what you meant.

### Arithmetic

Integer arithmetic is **checked**. Overflow and division by zero stop the program
with a message rather than producing a wrong answer or dying to a signal:

```
klang: integer overflow in '+'
klang: division by zero
```

This is not pedantry — signed overflow is *undefined behaviour* in C, so leaving it
alone means the optimizer is entitled to assume it never happens and miscompile
around it. `+ - * / %` and unary `-` are all covered, including the two cases that
are easy to forget: `INT64_MIN / -1` and `-INT64_MIN`.

**The cost is small and measured.** On a loop doing nothing but integer arithmetic —
the worst case there is — checked code runs about 5–15% slower than the same loop
written straight in C. Dividing by a literal that cannot be zero drops the check
entirely, so ordinary code keeps ordinary speed. Where the hardware exposes an
overflow flag the check is a single not-taken branch; a portable fallback keeps any
C99 compiler working.

When wrapping is what you actually mean — hashing, checksums — ask for it by name:

```kkg
wrapAdd(a, b)   wrapSub(a, b)   wrapMul(a, b)
```

Float arithmetic follows IEEE 754 and is not checked: infinities and NaN are the
defined answers there, not mistakes.

### Converting between int and float

There are no implicit numeric conversions — `1 + 2.5` is a compile error, so a
rounding decision is never made behind your back.

```kkg
toFloat(n)    // int -> float, always exact
toInt(x)      // float -> int, truncates toward zero
```

`toInt` saturates at the integer limits and maps NaN to zero, because the C cast it
replaces is undefined for those inputs.

### Closures

`fn(A, B) -> R` is a function type, and `|a, b| ...` is a closure. The body is either
one expression or a block. Parameter types can be left out whenever the surrounding
code already implies them.

```kkg
let inc = |x: int| x + 1
let sum = list.reduce(nums, 0, |acc, n| acc + n)   // types come from reduce
let log = |msg: string| { println("[app] ${msg}") }
```

A plain function can be used wherever a closure is expected, so `list.map(xs, double)`
works without wrapping.

**Capturing works exactly like passing an argument.** A closure takes a copy of what it
mentions from the enclosing scope, with the same mutability that variable had. So an
`int` is captured by value, and an array or map — being a reference — is captured as
that reference, which means changes through it are visible outside:

```kkg
let n = 10
let addN = |x: int| x + n        // n copied

let mut log: [string] = []
let record = |m: string| { push(log, m) }   // log is a reference; the caller sees pushes
```

There is one rule to learn, not two, and nothing can dangle: environments are
heap-allocated and collected, so a closure may outlive the call that created it.

```kkg
fn makeCounter(start: int) -> fn() -> int {
    let mut cell = [start]
    return || { cell[0] = cell[0] + 1; return cell[0] }
}
```

Closures nest, and an inner one may capture through an outer one. They can be stored in
arrays and maps like any other value.

**Evaluation is left to right.** C leaves the order between operands unspecified; Klang
does not, so `"${next()} ${next()}"` and `f(g(), h())` run in the order they are written.

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

## Modules

A file is a module. `import` names one by path, and the last segment of that path
becomes the name you call through:

```kkg
import "std/math"
import "std/list"
import "std/math" as arithmetic   // same module, second name

fn main() {
    println("${math.abs(0 - 7)}")
    println("${list.sum([1, 2, 3])}")
}
```

**Nothing is exported unless it says so.** `pub` marks a declaration usable from
another module; everything else is private to its own file. Private by default is
the safe direction: widening a module's surface later is harmless, narrowing it
breaks callers.

```kkg
pub fn abs(n: int) -> int { ... }   // callable as math.abs
fn isEven(n: int) -> bool { ... }   // std/math only; a compile error elsewhere
```

Rules that keep this predictable:

- **Import paths resolve from the project root**, not from the importing file, so a
  path always names the same module. Two files reaching `std/math` get *one* module,
  not two copies with duplicate declarations.
- Lookup order is: project root (the directory of the file you compiled), then each
  directory in `KLANG_PATH`, then next to the compiler — which is how the standard
  library is found without anyone configuring anything.
- **Imports come first**, before any declaration.
- A module is loaded once however many times it is imported, and an **import cycle is
  reported** rather than followed.
- Qualified names are the only way to reach another module, so an unqualified name can
  only ever mean something from your own file or the prelude. That is also what makes
  visibility checkable in one pass.

`Option`, `Result`, and generics all cross module boundaries unchanged — a
`pub fn first<T>(xs: [T]) -> Option<T>` in one module is instantiated per call site
in another, with no boxing.

## Calling C, and `unsafe`

Klang can call C, and does so without abandoning anything the type system promises
elsewhere. The rule is Rust's: **declaring a C function is fine; calling one is
`unsafe`.**

```kkg
extern header "<math.h>"
extern link "m"

extern fn cSqrt(x: float) -> float = "sqrt"   // = "..." when the C symbol differs
```

An `extern fn` has no body — the C symbol is the body. Declaring one is harmless.
Calling one is not, because the compiler cannot see what C will do with its
arguments, so the call must sit in an unsafe context:

```kkg
let r = unsafe { cSqrt(2.0) }        // take the obligation here
unsafe fn fast(x: float) -> float {  // or pass it to your callers
    return cSqrt(x)                  // an unsafe fn is already an unsafe context
}
```

**The point is the third option: discharge it once.** Wrap the unsafe primitive in a
function that checks what C cannot, and hand out something that cannot be misused:

```kkg
fn squareRoot(x: float) -> Result<float, string> {
    if x < 0.0 { return Err("no real square root of ${x}") }
    return Ok(unsafe { cSqrt(x) })
}
```

[std/fs](../std/fs.kkg) is the worked example. It calls `fopen`, `fgetc`, `fputs` and
`fclose`, turns null returns and error flags into `Result`, and exports only the safe
side — its externs are private, so callers cannot reach them even deliberately.
Reading a file is then ordinary Klang with no `unsafe` anywhere in sight.

Because `unsafe` is the only way in, `grep unsafe` over a project finds every place
the compiler stopped vouching for you. That is the whole value of the keyword.

Two details that keep the boundary honest:

- **A closure body is its own context.** Creating a closure inside `unsafe { ... }`
  does not make its body unsafe, because it may be called anywhere later.
- **An unsafe function cannot be passed as a value**, for the same reason — the call
  site would be invisible.

### Opaque handles

`extern type` declares a C pointer Klang can hold and pass back, but never open:

```kkg
extern type File
extern fn fopen(path: string, mode: string) -> File

if isNull(f) { return Err("cannot open '${path}'") }
```

There are no fields to read and no arithmetic to do, so the only thing you can do with
a handle is give it back to C. `isNull` is the one question you may ask about it.

### What crosses the boundary

`int` is `int64_t`, `float` is `double`, `bool` is `bool`, and `string` is a
NUL-terminated `char*`. Klang emits no prototypes of its own — the header you declare
is the single source of truth for a C function's signature, so the two can never drift
apart. `klangc` prints the libraries an `extern link` asked for.

One thing to know: a `string` handed to C points into the collected heap. It stays
alive for the duration of the call, but if C stores the pointer for later, keep a
Klang reference alive too.

## The collector

Klang ships its own garbage collector — not a dependency — because the runtime is
where latency, concurrency, and future WASM support are ultimately decided, and none
of those are steerable from behind someone else's allocator.

**Design: conservative mark-sweep.** *Conservative* means the collector does not ask
the compiler where the live pointers are. It scans the machine stack and the
callee-saved registers (spilled with `setjmp`), and treats any word that is exactly
the address of one of its objects as a reference.

The tradeoff, stated plainly:

- **Cost** — a non-pointer that happens to look like one keeps an object alive a bit
  longer than necessary, and objects can never be moved, which rules out a compacting
  collector later without changing this design.
- **Payoff** — no cooperation is needed from generated code. Every C local, every
  compiler temporary, and every value the optimizer parked in a register is covered
  automatically. The alternative, a precise shadow stack, needs every temporary
  registered and every early `return` to unwind it correctly — far more places to get
  it subtly wrong, and the failure mode is a freed live object rather than a retained
  dead one.

Details that matter:

- **Stack anchor.** The C `main` is a wrapper that records a stack address and then
  calls `klang_main`. Anchoring in an *enclosing* frame is what makes the scan range
  correct no matter how the C compiler orders locals — anchoring inside `klang_main`
  itself silently misses any local the compiler placed above the anchor.
- **String literals** live in `.rodata`, never in the heap, so they simply never
  appear in the object set. No interning pass is needed.
- **Object identification** is an exact-address hash set with a heap-range fast
  reject, so interior pointers are never followed — and never arise, since the one
  pointer stored inside an object (an array's data buffer) is kept at its base.
- **Pacing** is adaptive: collect when the heap exceeds `max(4 MB, 2 × live)`.

Two builtins make it observable, and `assert(cond, msg)` makes tests fail loudly:

```kkg
gcCollect()            // force a collection
let bytes = gcHeap()   // bytes currently held
```

[examples/gc.kkg](../examples/gc.kkg) is a regression test: it holds 500 nodes live
across 120k allocations and asserts every field survived. It is verified at `-O0`
through `-O3` and `-Os`, because conservative collection is exactly the kind of thing
that can pass at one optimization level and fail at another.

## Concurrency

`spawn e` runs `e` on another OS thread and hands back a `Task<T>`; `await` waits for
it and gives you the value.

```kkg
let a = spawn countWords(docs)
let b = spawn countLines(docs)
println("${await a} words, ${await b} lines")
```

**The rule is that nothing mutable is ever shared.** Numbers, booleans and strings are
immutable, so they cross for free. Arrays, maps, structs and enums are deep-copied on
the way in. Two threads therefore never hold a reference to the same object, and a data
race is not something to be careful about — it cannot be expressed.

This is deliberately a stronger promise than Rust makes. Rust prevents races by proving
that sharing is disciplined, which needs a borrow checker to be right and has
`unsafe impl Send` as an escape. Klang removes the sharing instead, so there is nothing
left to prove and no escape hatch to get wrong. The price is the copy, and it is a real
price — the design pays it deliberately rather than trading it for an analysis Klang
does not have.

Results come back **without** a copy. By the time `await` returns, the worker has
finished, so its result has exactly one owner again:

```kkg
fn titlesOf(docs: [Doc]) -> [string] { return docs.map(|d| d.title) }
let got = await spawn titlesOf(docs)     // no copy on the way out
```

Two things cannot cross, and the compiler says why:

- **a closure**, because it carries captured state that would be shared — pass the
  values it needs instead
- **an `extern type` handle**, because it points at something C owns, which Klang can
  neither copy nor reason about

### How the collector copes

A conservative collector has to see every thread's stack, and no thread may be moving
objects around while it sweeps. Threads agree to stop at **safepoints**: on allocation,
at loop back-edges, and around any blocking call. A thread that parks records where its
stack currently ends and spills its registers, so the collector scans it exactly as it
scans its own.

`await` parks before joining. Without that, a thread sitting in `pthread_join` would
never reach a safepoint and a collection started elsewhere would wait forever.

**Programs that never spawn pay nothing for any of this.** The thread runtime, the
locking and the safepoints are only emitted when the program actually contains a
`spawn`, so single-threaded code keeps exactly the performance it had.

## Implemented today (v0.11, Phase 10 complete)

**v0.1 core**
- `let`, `let mut`, immutability enforcement
- Types: `int`, `float`, `bool`, `string`
- `fn` with typed params, typed or omitted return type
- `if` / `else if` / `else`, `while`
- `struct` definitions, struct literals, field access
- Arithmetic (`+ - * / %`), comparison (`== != < <= > >=`), logical (`&& || !`), unary `-`
- String `+` concatenation and `==` / `!=` comparison
- `return`; builtins `println`, `print`, `toString`

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

**Phase 10 additions — concurrency, with nothing shared**
- `spawn e` -> `Task<T>` on a real OS thread, and `await` to join
- **Nothing mutable crosses a thread boundary**: immutables shared, mutables copied,
  so a data race cannot be written. Stronger than the move rule first planned, and
  stronger than Rust, which prevents races by proving sharing is disciplined
- Results come back without a copy — the worker is finished, so there is one owner
- Closures and extern handles are refused at the boundary, with the reason
- The collector stops the world at safepoints: allocation, loop back-edges, and
  around blocking calls; `await` parks before joining so a collection cannot hang
- pthreads and Win32 behind one interface; none of it emitted unless you spawn
- Measured: 2.4s of sequential work finishes in 0.93s across four tasks

**Phase 9 additions — arithmetic that cannot lie**
- **Checked integer arithmetic**: `+ - * / %` and unary `-` trap on overflow instead
  of wrapping, which in C is undefined behaviour rather than merely surprising
- **Division and remainder by zero** report themselves instead of raising a signal
- `INT64_MIN / -1` and `-INT64_MIN` covered, the two that are easy to miss
- Measured cost: 5–15% on a loop of pure arithmetic; a literal non-zero divisor
  drops its check entirely
- `wrapAdd` / `wrapSub` / `wrapMul` when wrapping is genuinely what you want
- `toInt` / `toFloat`, saturating rather than undefined at the edges

**Phase 8 additions — the C boundary, with the safety story intact**
- `extern fn`, with `= "symbol"` when the C name differs; `extern type` opaque handles
- `extern header` and `extern link`; klangc reports the libraries to link
- **Calling C is `unsafe`** — `unsafe { ... }` blocks and expressions, `unsafe fn`
- Enforced end to end: a closure body does not inherit unsafe, and an unsafe
  function cannot be passed as a value
- `isNull` for opaque handles; opaque types have no fields to reach into
- **`std/fs`** — real file I/O behind a fully safe `Result` API, with its externs
  private so callers cannot reach around it
- A function you define now shadows a builtin of the same name

**Phase 7 additions — the syntax, filled in and settled**
- **camelCase everywhere**: `toString`, `byteAt`, `fromByte`, `indexOf`, `gcCollect`,
  `gcHeap` renamed so the compiler and the standard library agree
- **Method calls**: `x.f(a)` is `f(x, a)`, with deterministic resolution and a clear
  message when two modules both match
- **`const`** at module level, `pub`-able, initializers may allocate
- **`break` and `continue`**, correct from inside a `match`, scoped to the function
- **Compound assignment** `+= -= *= /= %=`
- **Nesting block comments** `/* ... */`

**Phase 6 additions**
- **Closures**: `fn(A) -> R` types and `|a, b| ...` literals, block or expression bodied
- Parameter types inferred from the surrounding context; result inferred from the body
- **Capture by value, with the same mutability the variable had** — the same rule as
  passing an argument, so references stay shared and nothing can dangle
- Environments are GC-allocated, so a closure may outlive the call that built it
- Nested closures capture through the closures between them
- Plain functions usable as closure values; closures storable in arrays and maps
- Calling the result of any expression: `ops["add"](3, 4)`
- **Left-to-right evaluation order**, guaranteed where C leaves it unspecified
- `<` `<=` `>` `>=` on strings, so text can be sorted
- `std/list` gains map, filter, reduce, find, any, all, count, each, sorted

**Phase 5 additions**
- **Maps** `{K: V}` — literals, indexing, insert-on-assign, `has`, `remove`, `keys`, `values`
- `get(m, k)` returns `Option<V>`; reading a missing key aborts, as an array index does
- Map keys restricted to `int`, `string`, `bool`, rejected at compile time otherwise
- Open-addressed hash map emitted per (key, value) pair — no boxing, no function pointers
- **String primitives**: `substr`, `byteAt`, `fromByte`, `indexOf`
- **`std/string`** — split, join, replace, trim, case, pad, reverse, `parseInt` returning
  `Result` — all written in Klang, not built into the compiler

**Phase 4 additions**
- **Modules**: one file per module, `import "path"`, `as` for a second name
- **`pub`** — private by default; only marked declarations cross a module boundary
- Import paths resolve from the project root, so a path always names one module
- Standard library found next to the compiler, or via `KLANG_PATH`
- Import cycles and unknown members reported with the file and line that caused them
- A first standard library: `std/math` and `std/list`

**Phase 3 additions**
- **Klang's own garbage collector** — conservative mark-sweep over the machine stack
  and registers, with an adaptive collection threshold. Memory is now bounded: the
  benchmark that peaked at 126 MB before the GC now peaks at 18 MB.
- `gcCollect()` and `gcHeap()` to force a collection and read live bytes
- `assert(cond, msg)` — aborts with a message and a non-zero exit status

Generated C compiles clean under `-Wall -Wextra`.

## Not yet implemented

- Traits

- Recursive types (they need indirection, which the language does not have yet — the
  compiler rejects them with a clear message rather than looping)
- `toString` / `==` on structs and enums (match on them instead)
- Thread/channel send-ownership checking, `spawn`/`await`
- Any backend other than C (LLVM, WASM are not planned near-term — C gets us "every platform"
  for free via the host's C toolchain)
- Tooling: REPL, formatter, linter, package manager — all future work, not started
