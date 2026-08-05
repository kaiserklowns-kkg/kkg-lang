# The browser example

[../web.kkg](../web.kkg) is the whole application. This directory holds the page
it renders into, and the build output.

```sh
klangc examples/web.kkg -o examples/web/web.c
emcc -O2 examples/web/web.c --js-library examples/web/web.lib.js \
     -o examples/web/page.js -sALLOW_MEMORY_GROWTH -sEXPORTED_RUNTIME_METHODS=ccall
```

`klangc` writes two files: the C, and `web.lib.js` — the JavaScript half of the
`js fn` declarations in [std/dom](../../std/dom.kkg). It prints the emcc line you
need, so it does not have to be remembered.

Then serve this directory over http (a `file://` URL cannot fetch the wasm) and
open `index.html`:

```sh
python3 -m http.server -d examples/web 8000
```

`make test-web` does all of the above and then drives the page headlessly under
Node with a stub DOM, which is how this example is actually tested.
