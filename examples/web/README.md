# The browser example

[../web.kkg](../web.kkg) is the whole application — markup, styling, state,
routing, storage and the network. This directory holds only build output.

```sh
klangc web run examples/web.kkg
```

That compiles the program, builds it with `emcc`, serves it — with `bun` if it is
installed and `node` otherwise — and opens a browser. `--port` and `--no-open`
are there when you want them, and `klangc web build` stops after the build.

## There is no HTML file here, and no CSS file

`index.html`, `app.js` and `app.wasm` are all generated; none of them is
committed. The page shell is written by klangc, the markup comes from
[std/ui](../../std/ui.kkg) and the styling from [std/css](../../std/css.kkg),
both built as Klang values in `web.kkg`.

This directory exists at all because a program's page directory is where assets
would go — an image, a font — and because it gives the build output a stable home
next to the source. A project with no assets does not need one: `klangc new`
creates none, and the output lands under `.klang/`.

`make test-web` builds this and drives it headlessly under both Bun and Node
against a stub DOM and a stub server, which is how the example is tested.
