// examples/web.kkg driven headlessly, so "Klang can write a web application on
// its own" is checked by `make check` rather than asserted in a README.
//
// The stub parses the markup std/ui produces into a real tree — elements,
// attributes, parents — and supports the operations a diff performs, because the
// two things worth testing here are invisible in the output:
//
//   * re-rendering patches the DOM rather than replacing it, so a live <input>
//     keeps both its identity and what is being typed into it, and
//   * keys mean deleting from the middle of a list moves nothing else.
//
// Both are obvious to anyone using the page and impossible to see in a snapshot,
// which is why they are checked by element identity.
//
//   node tests/web_test.js path/to/app.js

const path = process.argv[2];
if (!path) {
  console.error("usage: node tests/web_test.js <app.js>");
  process.exit(2);
}

const { install } = require("./stub_dom.js");
const { app, q, all, byText, store, navigate, styleText } = install();

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
const show = (v) => (v && v.tag !== undefined ? `<${v.tag}>#${v.uid}` : JSON.stringify(v));
const fail = (m) => { failures++; realLog("  FAIL " + m); };
function check(what, got, want) {
  if (got !== want) fail(`${what}\n       got ${show(got)}\n       exp ${show(want)}`);
}
function contains(what, hay, needle) {
  if (!hay.includes(needle)) fail(`${what}\n       ${JSON.stringify(hay)}\n       lacks ${JSON.stringify(needle)}`);
}
function absent(what, hay, needle) {
  if (hay.includes(needle)) fail(`${what}: ${JSON.stringify(needle)} should not be there`);
}

// Stable ids, so a failure can say which element it actually got.
let uid = 0;
const stamp = () => { for (const n of app.walk()) if (n.uid === undefined) n.uid = ++uid; };

const rows = () => all().filter((n) => n.tag === "li");
const type = (t) => { q("#entry").value = t; };
const submitForm = () => app.fire("submit", all().find((n) => n.tag === "form"));
const click = (label) => app.fire("click", byText("button", label));
const clickIn = (row, label) =>
  app.fire("click", row.children.find((c) => c.textContent.trim() === label));
// `.row` is what std/css's base sheet calls a status line.
const status = () => all().find((n) => n.classes.has("row")).textContent;

const Module = require(path);

setTimeout(async () => {
  console.log = realLog;
  stamp();

  // ── the page came from Klang, not from a file ───────────────────────
  check("a heading exists", q("h1") !== null, true);
  check("heading text", q("h1").textContent, "Klang in the browser");
  check("the form is Klang-built", all().some((n) => n.tag === "form"), true);
  check("the input exists", q("#entry") !== null, true);
  contains("main logged", logs.join("\n"), "klang: ready, 3 tasks");

  // ── the stylesheet is Klang too ─────────────────────────────────────
  const sheet = styleText();
  contains("a plain rule", sheet, "max-width:34rem;");
  contains("a pseudo-class", sheet, "button:hover {");
  contains("a media query", sheet, "@media (max-width: 30rem) {");
  check("one style element", document.head.children.length, 1);

  // ── first load ──────────────────────────────────────────────────────
  check("three rows", rows().length, 3);
  contains("seeded", rows()[0].textContent, "write a language");
  contains("count", status(), "1 of 3 left");
  check("remaining()", Module._remaining(), 1);
  check("title", document.title, "Klang in the browser");

  // ── the diff leaves a live input alone ──────────────────────────────
  const entry = q("#entry");
  entry.value = "half typed";
  const firstRow = rows()[0];
  clickIn(rows()[2], "☐");                       // toggle the third task
  stamp();
  check("the input is the same element", q("#entry"), entry);
  check("what was typed survived", q("#entry").value, "half typed");
  check("row 1 was patched, not replaced", rows()[0], firstRow);
  check("toggled", Module._remaining(), 0);
  clickIn(rows()[2], "☑");
  check("toggled back", Module._remaining(), 1);

  // ── keys: deleting from the middle moves nothing else ───────────────
  const before = rows();
  const last = before[2];
  clickIn(before[0], "×");
  stamp();
  check("two rows left", rows().length, 2);
  check("the survivor is the same element", rows()[1], last);
  absent("row 1 gone", q("ul").textContent, "write a language");

  // ── the form, and a closure handler on submit ───────────────────────
  type("  ship it  ");
  submitForm();
  stamp();
  check("added, and trimmed", rows().length, 3);
  contains("new row", rows()[2].textContent, "ship it");
  check("entry cleared", q("#entry").value, "");
  check("focus returned", q("#entry").focused, true);

  type("   ");
  submitForm();
  check("blank refused", rows().length, 3);
  contains("blank message", status(), "type something first");

  // ── escaping is structural ──────────────────────────────────────────
  type("<script>alert(1)</script>");
  submitForm();
  stamp();
  check("no script element was created", all().some((n) => n.tag === "script"), false);
  contains("it arrived as text", q("ul").textContent, "<script>alert(1)</script>");

  // ── keyboard ────────────────────────────────────────────────────────
  // Delegated like every other event: one listener on the mount point, and the
  // input carries the handler index.
  type("half typed again");
  for (const fn of app.listeners.keydown || []) {
    fn({ target: q("#entry"), key: "Escape", preventDefault() {} });
  }
  check("escape cleared the box", q("#entry").value, "");

  // ── routing ─────────────────────────────────────────────────────────
  navigate("#done");
  stamp();
  contains("only done shown", q("ul").textContent, "give it a garbage collector");
  absent("todo hidden", q("ul").textContent, "ship it");
  navigate("#todo");
  contains("todo shown", q("ul").textContent, "ship it");
  absent("done hidden", q("ul").textContent, "give it a garbage collector");
  navigate("");
  check("back to all", rows().length, 4);

  // ── clear done ──────────────────────────────────────────────────────
  click("clear done");
  stamp();
  contains("cleared", status(), "cleared 1");
  click("clear done");
  contains("nothing left to clear", status(), "nothing to clear");

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
  click("sync");
  contains("says it is working", status(), "syncing");
  check("posted", lastRequest.init.method, "POST");
  check("to the right place", lastRequest.url, "/api/tasks");
  check("as json", lastRequest.init.headers["Content-Type"], "application/json");
  check("body is the task list", JSON.parse(lastRequest.init.body).length, 3);
  await settle();
  contains("reply reached Klang", status(), "synced 3");

  reply = { ok: false, status: 503, statusText: "Service Unavailable", body: "" };
  click("sync");
  await settle();
  contains("a bad status is a failure", status(), "503 Service Unavailable");

  reply = { throws: "network down" };
  click("sync");
  await settle();
  contains("a thrown request is a failure", status(), "network down");

  reply = { ok: true, status: 200, statusText: "OK", body: "not json at all" };
  click("sync");
  await settle();
  contains("garbage reply is handled", status(), "something odd");

  // ── churn, so a collection certainly runs through the middle ────────
  for (let i = 0; i < 400; i++) {
    type("task number " + i + " with a reasonably long title");
    submitForm();
  }
  check("after 400 adds", rows().length, 403);
  contains("last one intact", q("ul").textContent, "task number 399 with a reasonably long title");
  contains("first one intact", q("ul").textContent, "ship it");
  check("storage survived too", JSON.parse(store.get("klang.tasks")).length, 403);

  if (failures) {
    realLog(`web: ${failures} check(s) failed`);
    process.exit(1);
  }
  realLog("web: components, closures, a diff that keeps inputs and keys that keep rows — 403 tasks, all ok");
}, 200);
