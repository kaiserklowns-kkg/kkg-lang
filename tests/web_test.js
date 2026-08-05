// Runs examples/web.kkg headlessly, so the claim "Klang can drive a web page"
// is checked by `make check` rather than asserted in a README.
//
// The DOM here is a stub — just enough of querySelector, textContent, innerHTML,
// value and addEventListener for the page to work. That is the point: the test
// exercises the whole boundary (Klang -> js fn -> JavaScript, and JavaScript ->
// export fn -> Klang) without needing a browser.
//
//   node tests/web_test.js path/to/page.js

const path = process.argv[2];
if (!path) {
  console.error("usage: node tests/web_test.js <page.js>");
  process.exit(2);
}

class El {
  constructor(id) {
    this.id = id;
    this.textContent = "";
    this.innerHTML = "";
    this.value = "";
    this.attrs = {};
    this.classes = new Set();
    this.listeners = {};
    this.classList = {
      add: (c) => this.classes.add(c),
      remove: (c) => this.classes.delete(c),
    };
  }
  setAttribute(k, v) { this.attrs[k] = v; }
  addEventListener(ev, fn) { (this.listeners[ev] ||= []).push(fn); }
  fire(ev) { for (const fn of this.listeners[ev] || []) fn(); }
}

const els = {};
for (const id of ["#entry", "#add", "#list", "#count", "#message", "#complete", "#clear"]) {
  els[id] = new El(id);
}

globalThis.document = { querySelector: (sel) => els[sel] ?? null };

const logs = [];
const realLog = console.log;
console.log = (...a) => logs.push(a.join(" "));

let failures = 0;
function check(what, got, want) {
  if (got === want) return;
  failures++;
  realLog(`  FAIL ${what}\n       got:  ${JSON.stringify(got)}\n       want: ${JSON.stringify(want)}`);
}
function contains(what, hay, needle) {
  if (hay.includes(needle)) return;
  failures++;
  realLog(`  FAIL ${what}\n       ${JSON.stringify(hay)}\n       does not contain ${JSON.stringify(needle)}`);
}

const Module = require(path);

Module.onRuntimeInitialized = () => {};

// The module runs main() itself on load; give it a turn to finish, then drive it.
setTimeout(() => {
  console.log = realLog;

  // main() rendered three seeded tasks, two of them done.
  contains("initial render", els["#list"].innerHTML, "write a language");
  contains("initial render", els["#list"].innerHTML, "make it run in a browser");
  check("initial count", els["#count"].textContent, "1 of 3 left");
  check("remaining()", Module._remaining(), 1);
  contains("main logged", logs.join("\n"), "klang: page ready");

  // A click, exactly as the browser would deliver it.
  els["#entry"].value = "  ship it  ";
  els["#add"].fire("click");
  check("added", Module._remaining(), 2);
  check("count after add", els["#count"].textContent, "2 of 4 left");
  check("entry cleared", els["#entry"].value, "");
  contains("new row rendered", els["#list"].innerHTML, "ship it");

  // Empty input is refused rather than added.
  els["#entry"].value = "   ";
  els["#add"].fire("click");
  check("empty refused", Module._remaining(), 2);
  check("empty message", els["#message"].textContent, "type something first");

  // Escaping is the library's job, and it has to actually happen.
  els["#entry"].value = "<script>alert(1)</script>";
  els["#add"].fire("click");
  contains("escaped", els["#list"].innerHTML, "&lt;script&gt;");
  check("no raw tag", els["#list"].innerHTML.includes("<script>"), false);

  els["#complete"].fire("click");
  check("completed one", Module._remaining(), 2);

  els["#clear"].fire("click");
  check("cleared done", els["#count"].textContent, "2 of 2 left");
  check("clear message", els["#message"].textContent, "cleared 3");

  // A round trip through a string long enough to have been reallocated, after
  // enough churn to have triggered a collection — the point being that the
  // collector is precise now, and JavaScript's strings survive it.
  for (let i = 0; i < 400; i++) {
    els["#entry"].value = "task number " + i + " with a reasonably long title";
    els["#add"].fire("click");
  }
  check("after 400 adds", Module._remaining(), 402);
  contains("last one intact", els["#list"].innerHTML, "task number 399 with a reasonably long title");
  contains("first one intact", els["#list"].innerHTML, "ship it");

  if (failures) {
    realLog(`web: ${failures} check(s) failed`);
    process.exit(1);
  }
  realLog("web: page driven headlessly, 402 tasks, DOM and exports both ok");
}, 200);
