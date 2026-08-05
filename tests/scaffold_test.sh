#!/bin/sh
# `klangc new` claims to produce a project that already runs. This checks that,
# for all three kinds, in a scratch directory — because a scaffold that has
# rotted is worse than no scaffold: it fails on someone's first minute with the
# language.
#
#   tests/scaffold_test.sh path/to/klangc

set -e
KLANGC=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
HERE=$(pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

fail() { echo "  FAIL $1"; exit 1; }

# ── cli: it has to run, and say the right thing ───────────────────────
"$KLANGC" new demo-cli --kind cli > /dev/null
(cd demo-cli && "$KLANGC" run src/main.kkg > out.txt) || fail "cli did not run"
grep -q "1 of 2 left" demo-cli/out.txt || fail "cli printed something unexpected"
grep -q "parsed 42" demo-cli/out.txt || fail "cli's Result example did not work"
echo "  cli     runs"

# ── build: a binary the OS runs, with no trace of JavaScript ──────────
#
# Klang is a compiled language, and "compiled" has to mean something checkable:
# an executable file, and nothing of the web toolchain inside it.
(cd demo-cli && "$KLANGC" build src/main.kkg -o prog > /dev/null) || fail "build produced nothing"
[ -x demo-cli/prog ] || fail "the build output is not executable"
demo-cli/prog | grep -q "1 of 2 left" || fail "the built binary misbehaved"
grep -q "klang_js\|emscripten\|UTF8ToString" demo-cli/.klang/main.c \
  && fail "a program with no 'js fn' still carries JavaScript-boundary code"
echo "  build   an executable, with no JavaScript in it"

# ── server: it has to build, and the generated C has to be clean ──────
"$KLANGC" new demo-server --kind server > /dev/null
(cd demo-server && "$KLANGC" src/main.kkg -o out.c > /dev/null) || fail "server did not compile"
${CC:-cc} -std=c99 -Wall -Wextra -Werror -O1 -c demo-server/out.c -o /dev/null \
  || fail "the server template's generated C is not warning-clean"
echo "  server  builds"

# ── a web project is Klang and nothing else ───────────────────────────
#
# The whole point: no .html, no .css, no .js, no .c. The markup is std/ui and
# the styling std/css; klangc writes the page shell at build time.
"$KLANGC" new demo-web > /dev/null
strays=$(find demo-web -type f ! -name "*.kkg" ! -name "README.md" ! -name ".gitignore")
[ -z "$strays" ] || fail "a fresh web project should be Klang only, but has:
$strays"
grep -q 'std/ui' demo-web/src/main.kkg || fail "the template does not build its markup in Klang"
grep -q 'std/css' demo-web/src/main.kkg || fail "the template does not build its styling in Klang"
echo "  web     a fresh project is .kkg and nothing else"

# ── tailwind: the scaffold has to point Tailwind at the Klang source ──
"$KLANGC" new demo-tw --css tailwind > /dev/null
grep -q '@import "tailwindcss"' demo-tw/web/input.css || fail "tailwind input.css is not tailwind"
grep -q 'src/\*\*/\*\.kkg' demo-tw/web/input.css \
  || fail "tailwind would not see class names written in Klang strings"
grep -q '"tailwindcss"' demo-tw/package.json || fail "tailwind is not in package.json"
grep -q 'web/style.css' demo-tw/.gitignore || fail "generated css is not ignored"
echo "  css     plain and tailwind scaffolded"

# Actually compiling Tailwind means downloading it, so it is opt-in rather than
# something every `make check` does. KLANG_TEST_TAILWIND=1 turns it on.
if [ "${KLANG_TEST_TAILWIND:-}" = 1 ] && command -v emcc > /dev/null; then
  (cd demo-tw && "$KLANGC" web build > /dev/null) || fail "the tailwind project did not build"
  grep -q "rounded-full" demo-tw/web/style.css \
    || fail "a class used only in src/main.kkg did not reach the stylesheet"
  echo "  css     tailwind compiled, and it read the .kkg source"
fi

# ── web: it has to build, and the button has to work ──────────────────
if ! command -v emcc > /dev/null; then
  echo "  web     skipped (no emcc)"
  exit 0
fi
(cd demo-web && "$KLANGC" web build > /dev/null) || fail "web did not build"

JS=$(command -v bun || command -v node || true)
if [ -z "$JS" ]; then
  echo "  web     built (no bun or node, so not driven)"
  exit 0
fi

cat > drive.js <<EOF
// The scaffolded page, driven against the shared stub DOM: the markup is built by
// Klang, so this also checks that std/ui and its diff work in the template
// everyone starts from.
const { install } = require("$HERE/tests/stub_dom.js");
const { app, q, byText, styleText } = install();
const Module = require(process.argv[2]);
setTimeout(() => {
  const want = (got, exp, what) => {
    if (got !== exp) {
      console.log(\`  FAIL \${what}: \${JSON.stringify(got)} != \${JSON.stringify(exp)}\`);
      process.exit(1);
    }
  };
  want(q("h1") !== null, true, "the heading was built by Klang");
  want(styleText().includes("max-width:32rem;"), true, "the styling was built by Klang");
  want(q("#count").textContent, "0 clicks", "first render");
  app.fire("click", byText("button", "click me"));
  app.fire("click", byText("button", "click me"));
  want(q("#count").textContent, "2 clicks", "after two clicks");
  console.log("  web     runs — markup and styling both Klang, and the button counts");
}, 200);
EOF
# The build output lives under .klang/, because the project has no web/ of its
# own — it has no non-Klang files at all.
"$JS" drive.js "$WORK/demo-web/.klang/main/app.js"
