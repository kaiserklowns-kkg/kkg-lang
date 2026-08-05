// examples/ui.kkg driven headlessly. What is being checked is not "does it
// render" but the two things std/ui claims over building markup by hand:
//
//   * re-rendering patches the DOM rather than replacing it, so a live <input>
//     keeps both its identity and what is being typed into it, and
//   * keys mean deleting from the middle of a list moves nothing else.
//
// Both are invisible in the output and obvious to anyone using the page, which
// is exactly the kind of thing that has to be tested by identity.
const { install } = require("./stub_dom.js");
const { app, q, all, byText } = install();
const M = require(process.argv[2]);
const say = (s) => process.stdout.write(s + "\n");
const show = (v) => (v && v.tag !== undefined ? `<${v.tag} id=${v.id || "-"}>#${v.uid}` : JSON.stringify(v));
let uid = 0;
let bad = 0;
const want = (got, exp, what) => {
  if (got !== exp) { bad++; say(`  FAIL ${what}\n       got ${show(got)}\n       exp ${show(exp)}`); }
};
const stamp = () => { for (const n of app.walk()) if (n.uid === undefined) n.uid = ++uid; };

setTimeout(() => {
  stamp();
  want(q("h1") !== null, true, "h1 exists");
  want(q("h1").textContent, "Tasks", "heading");
  want(all().filter((n) => n.tag === "li").length, 2, "two rows");
  want(q("p").textContent, "2 tasks", "count");

  // The diff must leave a live input alone: type into it, force a re-render, and
  // both the element itself and what was typed have to survive.
  const entry = q("#entry");
  entry.value = "half typed";
  const rowsBefore = all().filter((n) => n.tag === "li");
  app.fire("click", byText("button", "todo"));
  stamp();
  want(q("#entry"), entry, "the input is the same element after re-render");
  want(q("#entry").value, "half typed", "what was typed survived");
  want(byText("button", "done") !== undefined, true, "the row toggled");
  want(all().filter((n) => n.tag === "li")[0], rowsBefore[0], "row 1 patched, not replaced");

  // Keyed removal: delete the first row; the second must be the same element.
  const rows = all().filter((n) => n.tag === "li");
  const second = rows[1];
  app.fire("click", rows[0].children.find((c) => c.textContent === "x"));
  stamp();
  const after = all().filter((n) => n.tag === "li");
  want(after.length, 1, "one row left");
  want(after[0], second, "the surviving row is the same element (keys worked)");

  // Adding goes through the form's submit handler, which is a closure.
  q("#entry").value = "third";
  app.fire("submit", all().find((n) => n.tag === "form"));
  stamp();
  want(all().filter((n) => n.tag === "li").length, 2, "added one");
  want(q("p").textContent, "2 tasks", "count updated");

  if (bad) { say(`ui: ${bad} failed`); process.exit(1); }
  say("ui: components, closures as handlers, and a diff that leaves inputs alone");
}, 200);
