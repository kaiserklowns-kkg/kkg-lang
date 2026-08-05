// Runs examples/web.kkg headlessly, so "Klang can write a web page on its own" is
// checked by `make check` rather than asserted in a README.
//
// The page is built by Klang as std/html values, so the stub here parses the
// markup it produces into a real tree — elements, attributes, data-*, parents.
// That is what makes the interesting parts testable: delegation has to find the
// right element, and escaping has to have happened.
//
//   node tests/web_test.js path/to/app.js

const path = process.argv[2];
if (!path) {
  console.error("usage: node tests/web_test.js <app.js>");
  process.exit(2);
}

const { install } = require("./stub_dom.js");
const { app, q, all, byText, store, navigate } = install();

// A stub server, so the network path is exercised rather than assumed. Both
// outcomes are scripted: `reply` decides what the next request gets.
let lastRequest = null;
let reply = { ok: true, status: 200, statusText: "OK", body: '{"saved":3}' };
globalThis.fetch = (url, init) => {
  lastRequest = { url, init };
  if (reply.throws) return Promise.reject(new Error(reply.throws));
  return Promise.resolve({
    ok: reply.ok, status: reply.status, statusText: reply.statusText,
    text: () => Promise.resolve(reply.body),
  });
};
const settle = () => new Promise((r) => setTimeout(r, 20));

const logs = [];
const realLog = console.log;
console.log = (...a) => logs.push(a.join(" "));
console.warn = (...a) => logs.push("WARN " + a.join(" "));

// ── assertions ─────────────────────────────────────────────────────────

let failures = 0;
const fail = (m) => { failures++; realLog("  FAIL " + m); };
function check(what, got, want) {
  if (got !== want) fail(`${what}\n       got:  ${JSON.stringify(got)}\n       want: ${JSON.stringify(want)}`);
}
function contains(what, hay, needle) {
  if (!hay.includes(needle)) fail(`${what}\n       ${JSON.stringify(hay)}\n       lacks ${JSON.stringify(needle)}`);
}
function absent(what, hay, needle) {
  if (hay.includes(needle)) fail(`${what}: ${JSON.stringify(needle)} should not be there`);
}

const type = (t) => { q("#entry").value = t; };
const submitForm = () => app.fire("submit", all().find((n) => n.tag === "form"));
// `html.arg(v)` renders as data-arg, which is what the delegated listener reads.
const rowButton = (kind, id) =>
  all().find((n) => n.classes.has(kind) && n.dataset.arg === String(id));
const clickRow = (kind, id) => app.fire("click", rowButton(kind, id));
const clickText = (t) => app.fire("click", byText("button", t));

const Module = require(path);

setTimeout(async () => {
  console.log = realLog;

  // ── the page came from Klang, not from a file ───────────────────────
  check("the shell mounted", q("#listbox") !== null, true);
  check("the form is Klang-built", [...app.walk()].some((n) => n.tag === "form"), true);
  check("the heading is Klang-built", q("h1") === null, false);
  check("heading text", q("h1").textContent, "Klang in the browser");
  check("the input exists", q("#entry") !== null, true);
  contains("main logged", logs.join("\n"), "klang: ready, 3 tasks");

  // ── first load: seeded, rendered, titled ────────────────────────────
  contains("initial render", q("#list").textContent, "write a language");
  contains("initial count", q("#status").textContent, "1 of 3 left");
  check("remaining()", Module._remaining(), 1);
  check("document.title", document.title, "Klang — 1 left");
  check("all filter is on", q("#filters").children[0].classes.has("on"), true);

  // ── the form, reached by delegation ─────────────────────────────────
  type("  ship it  ");
  submitForm();
  check("added", Module._remaining(), 2);
  contains("count after add", q("#status").textContent, "2 of 4 left");
  check("entry cleared", q("#entry").value, "");
  check("focus returned to the box", q("#entry").focused, true);
  contains("new row", q("#list").textContent, "ship it");

  type("   ");
  submitForm();
  check("blank refused", Module._remaining(), 2);
  contains("blank message", q("#status").textContent, "type something first");

  // ── escaping is structural: a text node cannot inject markup ────────
  type("<script>alert(1)</script>");
  submitForm();
  contains("escaped in the markup", q("#listbox").innerHTML, "&lt;script&gt;");
  absent("no raw tag reached the DOM", q("#listbox").innerHTML, "<script>");
  check("and it is one text node, not an element",
        [...app.walk()].some((n) => n.tag === "script"), false);

  // ── delegation: the handler travels with the element ────────────────
  clickRow("toggle", 3);
  check("toggled 3 done", Module._remaining(), 2);
  clickRow("toggle", 3);
  check("toggled 3 back", Module._remaining(), 3);

  clickRow("delete", 1);
  absent("row 1 gone", q("#list").textContent, "write a language");
  contains("count after delete", q("#status").textContent, "3 of 4 left");

  // ── keyboard ────────────────────────────────────────────────────────
  type("half typed");
  q("#entry").listeners.keydown[0]({ target: q("#entry"), key: "Escape" });
  check("escape cleared the box", q("#entry").value, "");

  // ── hash routing ────────────────────────────────────────────────────
  navigate("#done");
  check("done filter on", q("#filters").children[2].classes.has("on"), true);
  check("all filter off", q("#filters").children[0].classes.has("on"), false);
  contains("only done shown", q("#list").textContent, "give it a garbage collector");
  absent("todo hidden", q("#list").textContent, "ship it");

  navigate("#todo");
  contains("todo shown", q("#list").textContent, "ship it");
  absent("done hidden", q("#list").textContent, "give it a garbage collector");

  navigate("");
  check("back to all", q("#filters").children[0].classes.has("on"), true);

  // ── buttons that Klang mounted after the listener was installed ─────
  clickText("clear done");
  contains("clear message", q("#status").textContent, "cleared 1");
  contains("all remaining are todo", q("#status").textContent, "3 of 3 left");
  clickText("clear done");
  contains("nothing left to clear", q("#status").textContent, "nothing to clear");

  // ── it persisted, as JSON written by std/json ───────────────────────
  const saved = store.get("klang.tasks");
  if (!saved) fail("nothing was stored");
  else {
    const parsed = JSON.parse(saved);
    check("stored count", parsed.length, 3);
    contains("stored titles", saved, "ship it");
    check("stored shape", typeof parsed[0].id, "number");
  }

  // ── the network, and every way it can go wrong ──────────────────────
  clickText("sync");
  contains("says it is working", q("#status").textContent, "syncing");
  check("posted", lastRequest.init.method, "POST");
  check("to the right place", lastRequest.url, "/api/tasks");
  check("as json", lastRequest.init.headers["Content-Type"], "application/json");
  check("body is the task list", JSON.parse(lastRequest.init.body).length, 3);
  await settle();
  contains("reply reached Klang", q("#status").textContent, "synced 3");

  reply = { ok: false, status: 503, statusText: "Service Unavailable", body: "" };
  clickText("sync");
  await settle();
  contains("a bad status is a failure", q("#status").textContent, "503 Service Unavailable");

  reply = { throws: "network down" };
  clickText("sync");
  await settle();
  contains("a thrown request is a failure", q("#status").textContent, "network down");

  reply = { ok: true, status: 200, statusText: "OK", body: "not json at all" };
  clickText("sync");
  await settle();
  contains("garbage reply is handled", q("#status").textContent, "something odd");

  // ── churn, so a collection certainly runs through the middle ────────
  for (let i = 0; i < 400; i++) {
    type("task number " + i + " with a reasonably long title");
    submitForm();
  }
  check("after 400 adds", Module._remaining(), 403);
  contains("last one intact", q("#list").textContent, "task number 399 with a reasonably long title");
  contains("first one intact", q("#list").textContent, "ship it");
  check("storage survived too", JSON.parse(store.get("klang.tasks")).length, 403);

  if (failures) {
    realLog(`web: ${failures} check(s) failed`);
    process.exit(1);
  }
  realLog("web: page built in Klang — markup, delegation, routing, storage, network — 403 tasks, all ok");
}, 200);
