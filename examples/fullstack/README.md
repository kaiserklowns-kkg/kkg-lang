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

Klang compiles through C, and the browser half needs Emscripten. If you have
neither — on Windows, say — [tools/klangc.cmd](../../tools/klangc.cmd) runs the
real compiler inside Docker and passes your arguments through, so every command
above means the same thing. There is a `tools/klangc` for Unix hosts too.

```powershell
$env:Path += ";D:\Klowns-Language-Klang-\tools"
```

Then two terminals, because the two halves are two programs:

```powershell
# terminal 1 — the API, on 8099
klangc run examples\fullstack\api.kkg

# terminal 2 — the page, on 8080
klangc web run examples\fullstack\app.kkg --no-open
```

Open <http://localhost:8080>. Each wrapper publishes the port its command needs —
`run` gets 8099 and `web` gets 8080 — because two containers cannot both claim
the same one. `--no-open` is there because the container has no browser to open;
that part is yours. The compiler is built on first use, once, and skipped
afterwards unless `src/klangc.c` has changed.

Ctrl-C stops either of them.

Note that the `bin/klangc-linux` this produces is a Linux binary, and so is
anything `klangc build` writes. Build `src/klangc.c` with a Windows compiler if
you want a `klangc.exe` and native `.exe` output.

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
