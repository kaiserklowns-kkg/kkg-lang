# The browser example

[../web.kkg](../web.kkg) is the whole application. This directory holds the page
it renders into, and the build output.

```sh
klangc web run examples/web.kkg
```

That compiles the program, builds it with `emcc`, and serves it — with `bun` if
it is installed and `node` otherwise — then opens a browser. `--port` and
`--no-open` are there when you want them, and `klangc web build` stops after the
build if you would rather serve the files yourself.

The output is `app.js` and `app.wasm`, written beside the `index.html` here.
That page is used because it sits in a directory named after the program; a
project with no page of its own gets a plain generated one under `.klang/`.

`make test-web` builds this and then drives it headlessly under both Bun and Node
with a stub DOM, which is how the example is actually tested.

## What it is doing

There is no JavaScript in the application. [std/dom](../../std/dom.kkg) declares
the browser API as `js fn` — functions whose bodies are JavaScript — and wraps
them in a safe Klang surface. `web.kkg` keeps its state in module-level
`let mut`, renders with `dom.setHtml`, and its buttons call `export fn`s, which
is how JavaScript reaches back into Klang.
