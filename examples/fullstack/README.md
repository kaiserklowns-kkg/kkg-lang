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
