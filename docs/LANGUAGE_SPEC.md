# Klang Language Spec (v0.18)

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

`if` is also an expression, which is usually what you want when the alternative is
three lines and a `mut`:

```kkg
let label = if n < 0 { "negative" } else if n == 0 { "zero" } else { "positive" }
```

Two rules, and they are the same rule twice: **the braces hold one expression**, and
**`else` is required**. Klang has no block-value anywhere else — every function ends
in an explicit `return` — so a brace pair that quietly evaluated to its last statement
would be a second thing braces can mean. And without `else` there is no value when the
condition is false. Both branches must have the same type; neither may be void.

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
pushing to it, assigning into it, or removing from it requires `let mut`.

```kkg
remove(xs, 0)          // by index, bounds-checked, order preserved
```

Order is preserved rather than swapping the last element into the gap: a list that
reshuffles itself when you delete from it is a surprise nobody wants.

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

### Recursive types

A type may contain itself **through an array or a map**, because those are
references — the recursion goes through a pointer, so there is no infinite layout:

```kkg
enum Json {
    Null, Bool(bool), Num(float), Str(string),
    Arr([Json]),        // fine: an array is a reference
    Obj([Field]),
}
struct Field { key: string, value: Json }

struct Dir { name: string, files: [string], subdirs: [Dir] }
```

Containing yourself **by value** is still an error, and the message says how to fix it:

```
type Chain contains itself by value — put the recursive part behind an array,
as in [Chain], which is a reference
```

Mutual recursion works too: `Json` holds `[Field]` and `Field` holds `Json`.

This is what makes trees expressible at all — JSON documents, syntax trees,
directory listings — and it is why [std/json](../std/json.kkg) can exist.

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

### Raw memory

`pokeByte` and `peekByte` read and write bytes through an `extern type` pointer. This
is the sharp edge `unsafe` exists for, and it is how a Klang program builds a C struct
that no safe construct can express — a `sockaddr_in`, for instance, which is exactly
what [std/net](../std/net.kkg) does to bind a port:

```kkg
unsafe {
    let addr = calloc(16, 1)
    pokeByte(addr, 0, AF_INET)
    pokeByte(addr, 2, port / 256)
    pokeByte(addr, 3, port % 256)
    bind(fd, addr, 16)
}
```

Nothing checks that the offset is in range or that the layout is right — that is the
whole point of the keyword. Put it behind a safe wrapper and callers never see it.

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

## Calling JavaScript, and the browser

Klang compiles to WebAssembly, and WebAssembly on its own cannot touch a page. So
JavaScript is reachable the same way C is: declare it, and the compiler builds the
bridge. `js fn` is a function whose body is JavaScript rather than Klang.

```kkg
js fn setTitle(text: string) {
    document.title = text
}

js fn readNumber(sel: string) -> float {
    return Number(document.querySelector(sel).value)
}
```

The body is taken verbatim — it is not Klang, and the Klang lexer never looks at
it — so it is ordinary JavaScript, semicolons optional, comments and template
literals included. Nothing in it mentions pointers or `UTF8ToString`: the compiler
marshals both directions.

**Only `int`, `float`, `bool` and `string` cross.** An array, map, struct or enum
is a Klang heap object that means nothing on the other side, and the compiler says
so rather than marshalling something half-way. Numbers cross as JavaScript numbers,
which are float64, so an `int` outside ±2^53 stops with a message rather than
quietly rounding — the same standard the rest of Klang's arithmetic holds itself to.
Strings are copied, and the copy belongs to the collector.

**Calling a `js fn` is `unsafe`**, exactly as calling C is, and for the same reason:
Klang cannot check what happens on the other side. [std/dom](../std/dom.kkg) does
that unsafe work once and exports a safe API, so a page written against `std/dom`
contains no `unsafe` at all.

Going the other way, `export fn` makes a Klang function callable from JavaScript
under its own name:

```kkg
export fn remaining() -> int {
    return list.count(tasks, |t| !t.done)
}
```

JavaScript reaches it as `Module._remaining()`. The same four types apply, for the
same reason. A string argument or result crosses as a pointer, so a JavaScript
caller uses `Module.ccall("name", "string", ["string"], [...])`.

An event handler cannot be a Klang closure — the closure lives in Klang's heap and
JavaScript has no way to keep it alive — so a handler is named by string, and the
name is an `export fn`:

```kkg
dom.onSubmit("#form", "addTask")                       // takes nothing
dom.onKey("#entry", "keydown", "onKey")                // takes the key
dom.onValue("#search", "input", "onSearch")            // takes the field's value
dom.onChild("#list", "click", ".delete", "deleteRow")  // takes the row's data-id
```

The last one is delegation: one listener on the container, told which child was
hit, so a list of a thousand rows needs one handler rather than a thousand.

A string argument arriving this way is copied into the collected heap on the way
in. `Module.ccall` allocates it on the stack and releases it the moment the call
returns, so a handler that stored the pointer would be holding freed memory —
which it is not, because the boundary copies.

Beyond events, [std/dom](../std/dom.kkg) covers reading and writing text, HTML,
values, attributes and classes; `localStorage`; the hash and query string, with
`onHashChange` for routing; `setTimeout` and `setInterval`; and `escape`, because
forgetting to escape is the oldest bug on the web and a library should not leave
it to each caller.

### Module-level state

A page is event-driven: a handler runs long after `main` returned and cannot capture
a local. So state lives at module level. `const` is a value that never changes;
`let mut` is one that does.

```kkg
let mut tasks: [Task] = []
const PORT = 8080
```

A module-level `let` without `mut` is rejected, because it would say nothing `const`
does not.

### Starting, and running

```sh
klangc new myapp        # a project that already runs
cd myapp
klangc web run          # build it, serve it, open a browser
```

`klangc new` lays down a working program, not a folder of empty files — change a
line and see the change. Three kinds, one layout:

```
myapp/
  src/main.kkg      the program
  web/index.html    the page          (--kind web, the default)
  README.md         how to run this one
  .gitignore        what not to commit
```

`--kind cli` gives a console program and `--kind server` an HTTP server; both are
run with `klangc run`. All three are built and executed by `make check`, because
a scaffold that has rotted is worse than none — it fails on someone's first
minute with the language.

`klangc web run` is then the whole loop. It finds the program, compiles it,
builds it with `emcc`, serves it, and opens a browser.

- **The program** is the file you name, or `main.kkg` / `src/main.kkg` /
  `app.kkg` / `web.kkg` if you name none.
- **The page** is your `index.html` — beside the program, in a directory named
  after it, or in `web/`. Otherwise a plain one is generated under `.klang/`, so
  the first run works before you have written any HTML.
- **The server** is `bun` if it is installed and `node` otherwise. Both are
  supported and both are tested; `klangc` prints which one it used, because the
  two do differ and a silent choice is a worse surprise. The server itself is
  generated — there is no dependency to install and nothing to check in.
- `--port <n>` and `--no-open` do what they say. `klangc web build` stops after
  the build, for when you have a server of your own.

Underneath, `klangc` writes two files for a program that uses `js fn` — the C,
and a `.lib.js` holding the JavaScript half — and joins them with:

```sh
emcc -O2 app.c --js-library app.lib.js -o app.js -sALLOW_MEMORY_GROWTH \
     -sEXPORTED_RUNTIME_METHODS=ccall
```

`web run` prints that line as it runs it, so nothing about the build is hidden.

[examples/web.kkg](../examples/web.kkg) is a complete page — state, rendering,
events — with no JavaScript in it. `make test-web` builds it and drives it under
both Bun and Node against a stub DOM, so the claim is tested rather than asserted.

## The collector

Klang ships its own garbage collector — not a dependency — because the runtime is
where latency, concurrency, and future WASM support are ultimately decided, and none
of those are steerable from behind someone else's allocator.

**Design: precise mark-sweep.** Generated code hands the collector its roots; the
collector never guesses at them.

Each function declares a small array of *root slots* — one per parameter, named local
and compiler temporary that can hold a heap reference — and links it into a per-thread
list on entry, unlinking it at every exit including the early `return` that `?`
produces. Constants get one such frame that is never unlinked, since they live as long
as the program. A slot is an address *and a size*, so a struct holding three arrays
costs one slot rather than three: the bytes are still examined a word at a time, but
*which* bytes to examine is now exact.

This was not the original design. The collector began conservative — scanning the
machine stack and callee-saved registers and treating any word that happened to be an
object address as a reference — because that needs no cooperation from generated code
and so covers every temporary for free. It was rewritten because **WASM has no stack
the collector can read.** Locals there live in the WASM value stack and in indexed
locals, neither of which is addressable memory, so a conservative scan does not merely
lose precision — it finds nothing at all and frees live objects. Precision stopped
being the riskier option and became the only one.

The tradeoff that remains: objects still cannot be moved, so a compacting collector
would be a further change. The measured cost of rooting is within noise (the 120k
allocation regression test: 104 ms conservative, 107 ms precise).

**Collection happens only at safepoints, never inside allocation.** Allocation is the
one place that knows the heap grew, and collecting there is the obvious thing to do —
but it is wrong under a precise root set. A runtime helper such as `arr_new` allocates
a header and then allocates its buffer, and between the two the header exists only in
a C local of that helper, which is in no root frame; collecting inside the second
allocation frees it. So allocation only records the growth, and generated code polls
between statements and at loop back-edges — places where every live value sits in a
named local by construction and nothing is half-built. Overshoot between polls is
bounded by what a single statement can allocate.

Details that matter:

- **String literals** live in `.rodata`, never in the heap, so they simply never
  appear in the object set. No interning pass is needed.
- **Object identification** is an exact-address hash set with a heap-range fast
  reject, so interior pointers are never followed — and never arise, since the one
  pointer stored inside an object (an array's data buffer) is kept at its base.
- **Pacing** is adaptive: collect when the heap exceeds `max(4 MB, 2 × live)`.
- **The stack scan is still there** on targets that have a stack, as belt and braces.
  It is not load-bearing, and `-DK_PRECISE_ONLY=1` turns it off so that the root set
  can be tested on a machine where failure is debuggable.

Two builtins make it observable, and `assert(cond, msg)` makes tests fail loudly:

```kkg
gcCollect()            // force a collection
let bytes = gcHeap()   // bytes currently held
```

[examples/gc.kkg](../examples/gc.kkg) is a regression test: it holds 500 nodes live
across 120k allocations and asserts every field survived. It is verified at `-O0`
through `-O3` and `-Os`, because collection is exactly the kind of thing that can pass
at one optimization level and fail at another.

[tests/gc_stress.kkg](../tests/gc_stress.kkg) tests the root set specifically, and
`make test-gc` runs it in the only two configurations that prove anything:

- `-DK_PRECISE_ONLY=1` — the stack scan off, so the frames generated code hands over
  are all the collector has. This is the situation WASM imposes whether it is asked
  for or not.
- `-DK_PRECISE_ONLY=1 -DK_GC_STRESS=1` — the above, plus a collection forced at every
  single allocation, so a root that is missed for even one window is caught rather
  than surviving by luck. Ruinously slow, and the only honest test.

Both configurations found real bugs when they were first run, which is the argument
for keeping them in `make check`.

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

No thread may be building an object while another sweeps, so threads agree to stop at
**safepoints**: between statements, at loop back-edges, and around any blocking call.
A thread that parks hands over its root frames, and records where its stack currently
ends and spills its registers as well, so the collector examines it exactly as it does
its own.

`await` parks before joining. Without that, a thread sitting in `pthread_join` would
never reach a safepoint and a collection started elsewhere would wait forever.

**Programs that never spawn pay nothing for any of this.** The thread runtime, the
locking and the safepoints are only emitted when the program actually contains a
`spawn`, so single-threaded code keeps exactly the performance it had.

## Implemented today (v0.18, Phase 17 complete)

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

**Phase 17 additions — enough language to write a page**
- **`if` as an expression** — `let label = if n < 0 { "neg" } else { "pos" }`. Braces
  hold one expression and `else` is required, so braces keep meaning one thing.
- **`remove(xs, i)` on arrays**, bounds-checked and order-preserving. A list that
  could only grow was not a list.
- **std/dom is a real DOM library**: `onValue` / `onKey` / `onChild` / `onSubmit`
  hand a handler the one thing it needs; `onHashChange` for routing; localStorage;
  timers; classes, attributes, focus, counts; `escape` on the way into HTML.
- String arguments into an `export fn` are copied into the collected heap, because
  `ccall` frees its stack allocation the moment the call returns
- **[examples/web.kkg](../examples/web.kkg)** is now a real application — a form,
  event delegation over a list, hash routing, and state persisted as JSON through
  std/json — and `make test-web` drives every one of those against a stub DOM

**Phase 16 additions — a project, from nothing**
- **`klangc new <name>`** — a project that already runs. `--kind web` (default),
  `cli`, or `server`; one layout for all three: `src/main.kkg`, `web/index.html`,
  a README that says how to run this one, and a `.gitignore`.
- **`klangc run [file]`** — compile, build and execute a native program, so both
  kinds of program are reached the same way and neither is the awkward one.
  Exits with whatever the program exits with.
- `web run` now also finds `web/index.html`, which is the layout `new` lays down
- `make check` scaffolds all three kinds, runs the cli one, builds the server one
  warning-clean, and drives the web one's button — a rotted scaffold fails on
  someone's first minute with the language, so it is not left untested

**Phase 15 additions — one command to run a page**
- **`klangc web run`** — finds the program, compiles it, builds it with emcc,
  serves it, opens a browser. `web build` stops after the build.
- **Bun and Node are both supported**, Bun preferred, and `make test-web` runs the
  browser example under whichever of the two is installed — both, when both are
- The dev server is generated, not depended on: one small script that runs under
  either runtime, refuses `../` traversal, and sends `application/wasm` correctly
- Zero config — a lone `main.kkg` is enough. A project with no `index.html` gets a
  plain generated one so the first run works before any HTML has been written.

**Phase 14 additions — the browser**
- **`js fn`** — a function whose body is JavaScript, taken verbatim. The compiler
  marshals arguments and results, so no Klang source mentions a heap pointer.
- Only int, float, bool and string cross, and ints are checked for exactness rather
  than rounded into a float64. Calling one is `unsafe`, as calling C is.
- **`export fn`** — a Klang function JavaScript can call as `Module._name`
- **Module-level `let mut`** — state that outlives main, which an event handler needs
- **`std/dom`** — selectors, text, HTML, attributes, classes, events, escaping;
  written on top of `js fn`, and safe to use without `unsafe`
- `klangc` emits a companion `.lib.js` and prints the emcc command that joins them
- **[examples/web.kkg](../examples/web.kkg)** — a complete page with no JavaScript
  in it, tested headlessly under Node with a stub DOM by `make test-web`

**Phase 13 additions — a precise collector, and WASM that actually works**
- The collector no longer guesses at roots. Generated code declares a frame of root
  slots per function and links it into a per-thread list, unlinked at every exit
  including the early `return` `?` produces. Constants get a frame that never unlinks.
- Collection moved out of allocation and into safepoints at statement boundaries, so
  no runtime helper can be caught with a half-built object in a C local
- `-DK_PRECISE_ONLY=1` turns the stack scan off; `-DK_GC_STRESS=1` collects at every
  allocation. `make test-gc` runs both, which is how the root set is actually proven.
- **WASM works.** Every example except the two that need POSIX sockets or threads
  compiles with `emcc` and runs correctly under Node, including the GC stress test —
  where before, the 500-node live set was freed while still in use.
- Cost of precision, measured on the 120k-allocation test: 104 ms → 107 ms

**Phase 12 additions — recursive types, and JSON**
- A type may contain itself through an array or map, because those are references;
  by-value self-containment is still an error, with the fix in the message
- Mutual recursion works: Json holds [Field], Field holds Json
- Types are now emitted in three passes — names, then layouts in dependency order,
  then functions — so a name is always available before it is needed
- Variants of a `pub enum` you imported resolve unqualified, the way Some and Ok do
- **`std/json`** — parse and stringify, written in Klang with no FFI. Errors carry
  the byte offset. Exponent notation and  escapes are refused clearly rather than
  misread.
- The server now takes a JSON body and answers with a JSON value

**Phase 11 additions — backend: a real HTTP server**
- `pokeByte` / `peekByte`, unsafe-only, so a Klang program can build a C struct
- **`std/net`** — TCP listen, accept, connect, read, write over POSIX sockets,
  behind a `Result` API with no `unsafe` on the outside. POSIX only for now;
  Windows needs the Winsock variant, which wants its own module
- **`std/http`** — request parsing (method, path, query, headers, body) and
  response building, pure Klang with no unsafe at all
- [examples/server.kkg](../examples/server.kkg) serves HTML, JSON, query strings,
  POST bodies and 404s, and tests itself: a client task runs on another thread
  while the server accepts, so it needs no curl to prove it works
- Escape sequences fixed: carriage return, NUL and single quote were all missing,
  and an unknown escape used to silently drop the backslash — it is now an error

**Phase 10 additions — concurrency, with nothing shared**
- `spawn e` -> `Task<T>` on a real OS thread, and `await` to join
- **Nothing mutable crosses a thread boundary**: immutables shared, mutables copied,
  so a data race cannot be written. Stronger than the move rule first planned, and
  stronger than Rust, which prevents races by proving sharing is disciplined
- Results come back without a copy — the worker is finished, so there is one owner
- Closures and extern handles are refused at the boundary, with the reason
- The collector stops the world at safepoints: statement boundaries, loop back-edges,
  and around blocking calls; `await` parks before joining so a collection cannot hang
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
- **Klang's own garbage collector** — mark-sweep with an adaptive threshold, at that
  point conservative over the machine stack and registers. Memory is now bounded: the
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
- Any backend other than C (LLVM is not planned near-term — C gets us "every platform"
  for free via the host's C toolchain)
- Tooling: REPL, formatter, linter, package manager — all future work, not started
