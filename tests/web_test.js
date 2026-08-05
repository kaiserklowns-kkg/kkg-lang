// Runs examples/web.kkg headlessly, so the claim "Klang can drive a web page"
// is checked by `make check` rather than asserted in a README.
//
// The DOM here is a stub — querySelector, textContent, innerHTML, value,
// classList, dataset, closest, addEventListener, plus localStorage and
// location. That is the point: it exercises the whole boundary in both
// directions (Klang -> js fn -> JavaScript, and JavaScript -> export fn ->
// Klang) without needing a browser.
//
//   node tests/web_test.js path/to/app.js

const path = process.argv[2];
if (!path) {
  console.error("usage: node tests/web_test.js <app.js>");
  process.exit(2);
}

// ── the smallest DOM that can host this page ──────────────────────────

class El {
  constructor(sel) {
    this.sel = sel;
    this.id = sel.startsWith("#") ? sel.slice(1) : "";
    this.textContent = "";
    this.value = "";
    this.attrs = {};
    this.dataset = {};
    this.listeners = {};
    this.children = [];
    this.parent = null;
    const classes = new Set(sel.startsWith(".") ? [sel.slice(1)] : []);
    this.classes = classes;
    this.classList = {
      add: (c) => classes.add(c),
      remove: (c) => classes.delete(c),
      contains: (c) => classes.has(c),
    };
  }
  get innerHTML() { return this._html || ""; }
  // Parsing HTML is not the job here, but the page's own markup has to become
  // something clickable — so the rows it writes are turned into stub elements.
  set innerHTML(html) {
    this._html = html;
    this.children = [];
    const re = /<button class="(toggle|delete)" data-id="(\d+)"/g;
    let m;
    while ((m = re.exec(html)) !== null) {
      const b = new El("." + m[1]);
      b.dataset.id = m[2];
      b.parent = this;
      this.children.push(b);
    }
  }
  setAttribute(k, v) { this.attrs[k] = v; }
  removeAttribute(k) { delete this.attrs[k]; }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  focus() { this.focused = true; }
  remove() {}
  insertAdjacentHTML(_, html) { this.innerHTML = this.innerHTML + html; }
  contains(other) { return other === this || this.children.includes(other); }
  closest(sel) {
    let n = this;
    while (n) {
      if (sel.startsWith(".") && n.classes.has(sel.slice(1))) return n;
      if (sel.startsWith("#") && n.id === sel.slice(1)) return n;
      n = n.parent;
    }
    return null;
  }
  addEventListener(ev, fn) { (this.listeners[ev] ||= []).push(fn); }
  fire(ev, target) {
    for (const fn of this.listeners[ev] || []) fn({ target: target || this, preventDefault() {} });
  }
}

const els = {};
for (const sel of ["#form", "#entry", "#list", "#count", "#message", "#clear", "#sync",
                   "#f-all", "#f-todo", "#f-done"]) {
  els[sel] = new El(sel);
}

// A stub server, so the network path is exercised rather than assumed. Both
// outcomes are scripted: `reply` decides what the next request gets.
let lastRequest = null;
let reply = { ok: true, status: 200, statusText: "OK", body: '{"saved":3}' };
globalThis.fetch = (url, init) => {
  lastRequest = { url, init };
  if (reply.throws) return Promise.reject(new Error(reply.throws));
  return Promise.resolve({
    ok: reply.ok,
    status: reply.status,
    statusText: reply.statusText,
    text: () => Promise.resolve(reply.body),
  });
};
const settle = () => new Promise((r) => setTimeout(r, 20));

const store = new Map();
globalThis.document = { querySelector: (s) => els[s] ?? null, title: "" };
globalThis.localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => store.set(k, v),
  removeItem: (k) => store.delete(k),
};
globalThis.location = { pathname: "/", hash: "", search: "" };
const windowListeners = {};
globalThis.window = {
  addEventListener: (ev, fn) => (windowListeners[ev] ||= []).push(fn),
};
function navigate(hash) {
  location.hash = hash;
  for (const fn of windowListeners.hashchange || []) fn();
}

const logs = [];
const realLog = console.log;
console.log = (...a) => logs.push(a.join(" "));
console.warn = (...a) => logs.push("WARN " + a.join(" "));

// ── assertions ────────────────────────────────────────────────────────

let failures = 0;
function check(what, got, want) {
  if (got === want) return;
  failures++;
  realLog(`  FAIL ${what}\n       got:  ${JSON.stringify(got)}\n       want: ${JSON.stringify(want)}`);
}
function contains(what, hay, needle) {
  if (hay.includes(needle)) return;
  failures++;
  realLog(`  FAIL ${what}\n       ${JSON.stringify(hay)}\n       lacks ${JSON.stringify(needle)}`);
}
function absent(what, hay, needle) {
  if (!hay.includes(needle)) return;
  failures++;
  realLog(`  FAIL ${what}: ${JSON.stringify(needle)} should not be in ${JSON.stringify(hay)}`);
}

const submit = () => els["#form"].fire("submit");
const type = (t) => { els["#entry"].value = t; };
const rowButton = (kind, id) =>
  els["#list"].children.find((c) => c.classes.has(kind) && c.dataset.id === String(id));
const clickRow = (kind, id) => els["#list"].fire("click", rowButton(kind, id));

const Module = require(path);

setTimeout(async () => {
  console.log = realLog;

  // ── first load: seeded, rendered, titled ────────────────────────────
  contains("initial render", els["#list"].innerHTML, "write a language");
  check("initial count", els["#count"].textContent, "1 of 3 left");
  check("remaining()", Module._remaining(), 1);
  check("document.title", document.title, "Klang — 1 left");
  check("all filter is on", els["#f-all"].classes.has("on"), true);
  contains("main logged", logs.join("\n"), "klang: ready, 3 tasks");

  // ── the form ────────────────────────────────────────────────────────
  type("  ship it  ");
  submit();
  check("added", Module._remaining(), 2);
  check("count after add", els["#count"].textContent, "2 of 4 left");
  check("entry cleared", els["#entry"].value, "");
  check("focus returned to the box", els["#entry"].focused, true);
  contains("new row", els["#list"].innerHTML, "ship it");

  type("   ");
  submit();
  check("blank refused", Module._remaining(), 2);
  check("blank message", els["#message"].textContent, "type something first");

  // ── escaping, which is not optional ─────────────────────────────────
  type("<script>alert(1)</script>");
  submit();
  contains("escaped", els["#list"].innerHTML, "&lt;script&gt;");
  absent("no raw tag", els["#list"].innerHTML, "<script>");

  // ── delegation: one listener, the right row ─────────────────────────
  clickRow("toggle", 3);
  check("toggled 3 done", Module._remaining(), 2);
  clickRow("toggle", 3);
  check("toggled 3 back", Module._remaining(), 3);

  const before = els["#list"].children.length;
  clickRow("delete", 1);
  check("one row gone", els["#list"].children.length, before - 2);   // two buttons per row
  absent("row 1 gone", els["#list"].innerHTML, "write a language");
  check("count after delete", els["#count"].textContent, "3 of 4 left");

  // ── keyboard ────────────────────────────────────────────────────────
  type("half typed");
  els["#entry"].fire("keydown");   // stub passes no key
  els["#entry"].listeners.keydown[0]({ target: els["#entry"], key: "Escape" });
  check("escape cleared the box", els["#entry"].value, "");

  // ── hash routing ────────────────────────────────────────────────────
  navigate("#done");
  check("done filter on", els["#f-done"].classes.has("on"), true);
  check("all filter off", els["#f-all"].classes.has("on"), false);
  contains("only done shown", els["#list"].innerHTML, "give it a garbage collector");
  absent("todo hidden", els["#list"].innerHTML, "ship it");

  navigate("#todo");
  contains("todo shown", els["#list"].innerHTML, "ship it");
  absent("done hidden", els["#list"].innerHTML, "give it a garbage collector");

  navigate("");
  check("back to all", els["#f-all"].classes.has("on"), true);

  // ── clear done, and array removal ───────────────────────────────────
  els["#clear"].fire("click");
  check("clear message", els["#message"].textContent, "cleared 1");
  check("all remaining are todo", els["#count"].textContent, "3 of 3 left");
  els["#clear"].fire("click");
  check("nothing left to clear", els["#message"].textContent, "nothing to clear");

  // ── it persisted, as JSON written by std/json ───────────────────────
  const saved = store.get("klang.tasks");
  if (!saved) { failures++; realLog("  FAIL nothing was stored"); }
  else {
    const parsed = JSON.parse(saved);
    check("stored count", parsed.length, 3);
    contains("stored titles", saved, "ship it");
    check("stored shape", typeof parsed[0].id, "number");
  }

  // ── the network, both ways it can go ────────────────────────────────
  els["#sync"].fire("click");
  check("says it is working", els["#message"].textContent, "syncing…");
  check("posted", lastRequest.init.method, "POST");
  check("to the right place", lastRequest.url, "/api/tasks");
  check("as json", lastRequest.init.headers["Content-Type"], "application/json");
  check("body is the task list", JSON.parse(lastRequest.init.body).length, 3);
  await settle();
  check("reply reached Klang", els["#message"].textContent, "synced 3");

  reply = { ok: false, status: 503, statusText: "Service Unavailable", body: "" };
  els["#sync"].fire("click");
  await settle();
  check("a bad status is a failure", els["#message"].textContent,
        "sync failed: 503 Service Unavailable");

  reply = { throws: "network down" };
  els["#sync"].fire("click");
  await settle();
  check("a thrown request is a failure", els["#message"].textContent,
        "sync failed: network down");

  reply = { ok: true, status: 200, statusText: "OK", body: "not json at all" };
  els["#sync"].fire("click");
  await settle();
  contains("garbage reply is handled", els["#message"].textContent, "server sent something odd");

  // ── churn, so a collection certainly runs through the middle ────────
  for (let i = 0; i < 400; i++) {
    type("task number " + i + " with a reasonably long title");
    submit();
  }
  check("after 400 adds", Module._remaining(), 403);
  contains("last one intact", els["#list"].innerHTML, "task number 399 with a reasonably long title");
  contains("first one intact", els["#list"].innerHTML, "ship it");
  check("storage survived too", JSON.parse(store.get("klang.tasks")).length, 403);

  if (failures) {
    realLog(`web: ${failures} check(s) failed`);
    process.exit(1);
  }
  realLog("web: form, delegation, routing, storage, keyboard, network — 403 tasks, all ok");
}, 200);
