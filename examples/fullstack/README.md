# Klang, end to end

Two programs, one language, two targets.

- [api.kkg](api.kkg) → a **native executable**. A REST service over POSIX
  sockets: `GET`/`POST`/`PUT`/`DELETE` on `/api/tasks`, plus `/api/health`.
- [app.kkg](app.kkg) → **WebAssembly**. The page, talking to it over HTTP.

Neither knows the other is Klang. They meet at JSON over TCP, the same as they
would with anything else on the other end.

## Running it

```sh
klangc build examples/fullstack/api.kkg -o api
./api &                                       # http://127.0.0.1:8099

klangc web run examples/fullstack/app.kkg     # http://localhost:8080
```

### Without a toolchain installed

Klang needs a C compiler, and the browser half needs Emscripten. If you have
neither — on Windows, say — Docker has both, and the two ports are published so
the browser on your own machine reaches them:

```sh
docker run --rm -p 8080:8080 -p 8099:8099 -v "$PWD":/w -w /w emscripten/emsdk bash -c '
  gcc -std=c99 -O2 -o bin/klangc src/klangc.c -lm
  ./bin/klangc build examples/fullstack/api.kkg -o /tmp/api
  /tmp/api &
  ./bin/klangc web run examples/fullstack/app.kkg --no-open --port 8080'
```

Then open <http://localhost:8080>. The first run compiles the compiler and both
programs, so give it half a minute. `--no-open` is there because the container
has no browser to open — that part is yours.

Note that `bin/klangc` built this way is a Linux binary. Build it again with a
Windows compiler if you want to run `klangc` outside the container.

The page is served from one port and the API answers on another, so the browser
asks permission first. `api.kkg` answers the preflight and puts the CORS headers
on every reply — including the errors, because a 404 the browser refuses to read
is a 404 nobody can debug.

## Testing it

```sh
make test-fullstack
```

Nothing is stubbed. The API runs as a real binary, the page runs as real
WebAssembly, and the test drives the page with node's real `fetch`, then asks the
server directly whether it agrees. A failure could be in the UI, the diff, the
JSON, the HTTP parser or the socket — which is exactly why the test is worth
having.

Passes under both Bun and Node.

## What it exercises

| | |
|---|---|
| UI | components, closures as handlers, a keyed list, a diff that leaves the input alone |
| Systems | a native binary, POSIX sockets, the garbage collector under load |
| API | request parsing, routing by method and path, status codes, CORS |
| Both | `std/json`, written in Klang, parsing and serialising on each side |
