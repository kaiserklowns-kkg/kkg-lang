// A DOM small enough to read, faithful enough to test against.
//
// Klang builds its markup as std/html values and mounts the rendered string, so a
// stub that only holds a few fixed elements would not exercise anything
// interesting. This one parses what Klang produced into a real tree — elements,
// attributes, data-*, parents — which is what makes delegation and escaping
// testable: a delegated listener has to find the right ancestor, and an escaped
// text node has to be text rather than an element.
//
// Shared by tests/web_test.js and tests/scaffold_test.sh.

const VOID = new Set(["area", "base", "br", "col", "embed", "hr", "img", "input",
                      "link", "meta", "source", "track", "wbr"]);

class El {
  constructor(tag = "div", attrs = {}) {
    this.tag = tag;
    this.attrs = attrs;
    this.children = [];
    this.parent = null;
    this.listeners = {};
    this.textParts = [];
    this.value = attrs.value ?? "";
    this.type = attrs.type ?? "";
    this.dataset = {};
    for (const [k, v] of Object.entries(attrs)) {
      if (k.startsWith("data-")) {
        this.dataset[k.slice(5).replace(/-(.)/g, (_, c) => c.toUpperCase())] = v;
      }
    }
    const self = this;
    this.classList = {
      add: (c) => { self.attrs.class = ((self.attrs.class || "") + " " + c).trim(); },
      remove: (c) => {
        self.attrs.class = (self.attrs.class || "")
          .split(/\s+/).filter((x) => x && x !== c).join(" ");
      },
      contains: (c) => (self.attrs.class || "").split(/\s+/).includes(c),
    };
  }
  get id() { return this.attrs.id || ""; }
  get classes() { return new Set((this.attrs.class || "").split(/\s+/).filter(Boolean)); }
  get textContent() {
    return this.textParts.join("") + this.children.map((c) => c.textContent).join("");
  }
  set textContent(t) { this.children = []; this.textParts = [t]; this._html = null; }
  get innerHTML() { return this._html ?? ""; }
  set innerHTML(html) {
    this._html = html;
    this.children = [];
    this.textParts = [];
    parseInto(this, html);
  }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  setAttribute(k, v) { this.attrs[k] = v; }
  removeAttribute(k) { delete this.attrs[k]; }
  focus() { this.focused = true; }
  remove() {}
  insertAdjacentHTML(_, html) { this.innerHTML = this.innerHTML + html; }
  contains(other) {
    for (let n = other; n; n = n.parent) if (n === this) return true;
    return false;
  }
  closest(sel) {
    for (let n = this; n; n = n.parent) if (matches(n, sel)) return n;
    return null;
  }
  addEventListener(ev, fn) { (this.listeners[ev] ||= []).push(fn); }
  fire(ev, target) {
    for (const fn of this.listeners[ev] || []) {
      fn({ target: target || this, preventDefault() {} });
    }
  }
  *walk() { yield this; for (const c of this.children) yield* c.walk(); }
}

function matches(el, sel) {
  if (sel.startsWith("#")) return el.id === sel.slice(1);
  if (sel.startsWith(".")) return el.classes.has(sel.slice(1));
  if (sel.startsWith("[") && sel.endsWith("]")) return el.getAttribute(sel.slice(1, -1)) !== null;
  return el.tag === sel;
}

// Only has to read what std/html writes: well-formed tags, quoted attributes,
// void elements unclosed, and text between them.
function parseInto(root, html) {
  const stack = [root];
  let i = 0;
  while (i < html.length) {
    if (html[i] !== "<") {
      const next = html.indexOf("<", i);
      const end = next === -1 ? html.length : next;
      stack[stack.length - 1].textParts.push(html.slice(i, end));
      i = end;
      continue;
    }
    if (html[i + 1] === "/") {
      if (stack.length > 1) stack.pop();
      i = html.indexOf(">", i) + 1;
      continue;
    }
    const close = html.indexOf(">", i);
    const inner = html.slice(i + 1, close);
    const sp = inner.search(/\s/);
    const tag = (sp === -1 ? inner : inner.slice(0, sp)).toLowerCase();
    const attrs = {};
    if (sp !== -1) {
      const re = /([a-zA-Z0-9_:\-]+)="([^"]*)"/g;
      let m;
      while ((m = re.exec(inner.slice(sp))) !== null) attrs[m[1]] = m[2];
    }
    const el = new El(tag, attrs);
    el.parent = stack[stack.length - 1];
    el.parent.children.push(el);
    if (!VOID.has(tag)) stack.push(el);
    i = close + 1;
  }
}

/* Installs the globals a Klang page expects and hands back the handles a test
   needs. Call before requiring the built module. */
function install() {
  const app = new El("div", { id: "app" });
  const store = new Map();
  const windowListeners = {};

  // std/css renders into a <style> that std/dom appends to the head, so the head
  // is real enough here for the stylesheet to be inspected.
  const head = new El("head");
  globalThis.document = {
    title: "",
    head,
    querySelector: (sel) => {
      for (const root of [app, head]) {
        for (const n of root.walk()) if (matches(n, sel)) return n;
      }
      return null;
    },
    getElementById: (id) => globalThis.document.querySelector("#" + id),
    createElement: (tag) => new El(tag),
  };
  head.appendChild = (el) => { el.parent = head; head.children.push(el); };
  globalThis.localStorage = {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => store.set(k, v),
    removeItem: (k) => store.delete(k),
  };
  globalThis.location = { pathname: "/", hash: "", search: "" };
  globalThis.window = {
    addEventListener: (ev, fn) => (windowListeners[ev] ||= []).push(fn),
  };

  const q = (sel) => globalThis.document.querySelector(sel);
  return {
    app, head, store,
    q,
    styleText: () => head.children.filter((c) => c.tag === "style")
                                  .map((c) => c.textContent).join(""),
    all: () => [...app.walk()],
    byText: (tag, t) => [...app.walk()].find((n) => n.tag === tag && n.textContent.trim() === t),
    navigate: (hash) => {
      globalThis.location.hash = hash;
      for (const fn of windowListeners.hashchange || []) fn();
    },
  };
}

module.exports = { El, install, matches };
