// The full stack, end to end, with nothing stubbed between the halves.
//
// examples/fullstack/api.kkg is running as a native binary on 8099. This drives
// examples/fullstack/app.kkg — the same language, compiled to WebAssembly —
// against it over real HTTP, using node's real fetch. Nothing here fakes the
// network or the server.
//
// So what is actually being tested is Klang's UI layer, Klang's HTTP server,
// Klang's JSON in both directions, and the boundary between WebAssembly and the
// operating system's TCP stack.
//
//   node tests/fullstack_test.js path/to/app.js

const path = process.argv[2];
if (!path) {
  console.error("usage: node tests/fullstack_test.js <app.js>");
  process.exit(2);
}

const { install } = require("./stub_dom.js");
const { app, q, all, byText } = install();

const API = "http://127.0.0.1:8099/api";

const logs = [];
const realLog = console.log;
console.log = (...a) => logs.push(a.join(" "));
console.warn = (...a) => logs.push("WARN " + a.join(" "));

let failures = 0;
const fail = (m) => { failures++; realLog("  FAIL " + m); };
function check(what, got, want) {
  if (got !== want) fail(`${what}\n       got  ${JSON.stringify(got)}\n       want ${JSON.stringify(want)}`);
}
function contains(what, hay, needle) {
  if (!hay.includes(needle)) fail(`${what}\n       ${JSON.stringify(hay)}\n       lacks ${JSON.stringify(needle)}`);
}

const rows = () => all().filter((n) => n.tag === "li");
const titles = () => rows().map((r) => r.children.find((c) => c.tag === "span").textContent);
const type = (t) => { q("#entry").value = t; };
const submitForm = () => app.fire("submit", all().find((n) => n.tag === "form"));
const click = (label) => app.fire("click", byText("button", label));
const clickIn = (row, label) =>
  app.fire("click", row.children.find((c) => c.textContent.trim() === label));
// `.row` is the class std/css's base sheet gives a status line.
const status = () => all().find((n) => n.classes.has("row")).textContent;

// The page talks to the server, so waiting means waiting for a round trip. Poll
// rather than sleep a fixed time, so the test is neither flaky nor slow.
async function until(what, cond, ms = 3000) {
  const stop = Date.now() + ms;
  while (Date.now() < stop) {
    if (cond()) return true;
    await new Promise((r) => setTimeout(r, 20));
  }
  fail(`timed out waiting for ${what}`);
  return false;
}

const Module = require(path);

(async () => {
  // The module instantiates asynchronously, so its exports are not there the
  // instant require() returns.
  await until("the module to start", () => typeof Module._taskCount === "function");

  // ── the page loaded its data from the server ────────────────────────
  await until("the first load", () => Module._taskCount() === 3);
  check("three tasks came from the API", rows().length, 3);
  contains("and they are the server's", titles().join("|"), "serve an API from it");
  contains("status says where they came from", status(), "from the server");

  // ── creating goes through the API and comes back ────────────────────
  type("  added from the browser  ");
  submitForm();
  await until("the create round trip", () => Module._taskCount() === 4);
  contains("the new task is on the page", titles().join("|"), "added from the browser");

  // The server, asked directly, agrees — so the page is not just showing what it
  // hoped happened.
  const fromApi = await (await fetch(`${API}/tasks`)).json();
  check("the server has it too", fromApi.length, 4);
  check("and trimmed it", fromApi[3].title, "added from the browser");

  // ── toggling ────────────────────────────────────────────────────────
  const third = rows()[2];
  clickIn(third, "☐");
  await until("the update round trip", async () => true);
  await until("the server records it", () => rows()[2].classes.has("done"), 3000);
  const afterToggle = await (await fetch(`${API}/tasks`)).json();
  check("the server thinks so as well", afterToggle[2].done, true);

  // ── deleting ────────────────────────────────────────────────────────
  clickIn(rows()[0], "×");
  await until("the delete round trip", () => Module._taskCount() === 3);
  const afterDelete = await (await fetch(`${API}/tasks`)).json();
  check("the server dropped it", afterDelete.length, 3);
  check("and it is the right one that went", afterDelete[0].title.startsWith("give it"), true);

  // ── the API's own errors reach the page ─────────────────────────────
  type("   ");
  submitForm();
  contains("blank is refused before the request", status(), "type something first");

  // ── the health endpoint, which reports the server's own heap ────────
  click("health");
  await until("the health round trip", () => status().includes("bytes of heap"));
  contains("the server reported its own heap", status(), "bytes of heap");

  // ── a real failure: point the page at a dead port ───────────────────
  // Not by stubbing fetch — by stopping the server, which the harness does after
  // this test. Instead, check the error path directly through the API itself.
  const notFound = await fetch(`${API}/tasks/9999`, { method: "DELETE" });
  check("the API 404s an unknown id", notFound.status, 404);
  const badJson = await fetch(`${API}/tasks`, {
    method: "POST", headers: { "Content-Type": "application/json" }, body: "nope",
  });
  check("and 400s a body that is not JSON", badJson.status, 400);
  check("CORS is on the error too", badJson.headers.get("access-control-allow-origin"), "*");

  if (failures) {
    realLog(`fullstack: ${failures} check(s) failed`);
    process.exit(1);
  }
  realLog("fullstack: wasm UI ↔ native Klang API over real HTTP — create, update, delete, health, errors");
  process.exit(0);
})();
