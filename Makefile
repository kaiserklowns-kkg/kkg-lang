CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
# Examples that call C may need libraries; klangc prints what they are.
LDLIBS ?= -lm -lpthread
EXAMPLES := hello basics phase1 phase2 gc modules phase5 phase6 phase7 phase8 phase9 phase10 server json

all: bin/klangc

bin/klangc: src/klangc.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o bin/klangc src/klangc.c

# Compile each example to C, build it, run it. The generated C must build clean.
.PHONY: test
test: bin/klangc
	@set -e; for f in $(EXAMPLES); do \
		echo "=== $$f ==="; \
		./bin/klangc examples/$$f.kkg -o /tmp/klang_$$f.c >/dev/null; \
		$(CC) $(CFLAGS) -o /tmp/klang_$$f /tmp/klang_$$f.c $(LDLIBS); \
		/tmp/klang_$$f; \
	done

# The root set, checked the only way that proves anything: with the conservative
# stack scan switched off, so the frames generated code hands over are all the
# collector has — the situation WASM puts it in whether it likes it or not — and
# with a collection forced at every allocation, so a missed root cannot slip
# through the window between one and the next.
.PHONY: test-gc
test-gc: bin/klangc
	@./bin/klangc tests/gc_stress.kkg -o /tmp/klang_gcs.c >/dev/null
	@echo "=== gc: precise roots only ==="
	@$(CC) $(CFLAGS) -DK_PRECISE_ONLY=1 -o /tmp/klang_gcs /tmp/klang_gcs.c $(LDLIBS) && /tmp/klang_gcs
	@echo "=== gc: precise roots only, collecting at every allocation ==="
	@$(CC) $(CFLAGS) -DK_PRECISE_ONLY=1 -DK_GC_STRESS=1 -o /tmp/klang_gcs /tmp/klang_gcs.c $(LDLIBS) && /tmp/klang_gcs

# `klangc new` claims to produce a project that already runs, so that gets
# checked. A rotted scaffold is worse than none: it fails on someone's first
# minute with the language. The web third skips itself without emcc.
.PHONY: test-new
test-new: bin/klangc
	@echo "=== klangc new ==="
	@sh tests/scaffold_test.sh bin/klangc

# The browser example, driven headlessly. Needs emcc and a JS runtime, so it is
# not part of `check` — but when they are present there is no excuse for skipping
# it. Bun and Node both have to work, so whichever is installed gets used, and if
# both are, both run.
.PHONY: test-web
test-web: bin/klangc
	@command -v emcc >/dev/null || { echo "skipping test-web: emcc not installed"; exit 0; }
	@./bin/klangc web build examples/web.kkg
	@ran=0; \
	for js in bun node; do \
		command -v $$js >/dev/null || continue; \
		printf '%-5s ' "$$js"; $$js tests/web_test.js "$$PWD/examples/web/app.js" || exit 1; \
		ran=1; \
	done; \
	[ $$ran = 1 ] || echo "skipping test-web: neither bun nor node installed"

# The full stack, with nothing stubbed between the halves: the API runs as a
# native binary and the page runs as WebAssembly, talking to it over real HTTP.
# Both are Klang. This is the one test where a failure could be in the UI, the
# server, the JSON, or the socket — which is the point of having it.
.PHONY: test-fullstack
test-fullstack: bin/klangc
	@command -v emcc >/dev/null || { echo "skipping test-fullstack: emcc not installed"; exit 0; }
	@js=$$(command -v bun || command -v node || true); \
	if [ -z "$$js" ]; then echo "skipping test-fullstack: neither bun nor node"; exit 0; fi; \
	./bin/klangc build examples/fullstack/api.kkg -o /tmp/klang_api > /dev/null; \
	./bin/klangc web build examples/fullstack/app.kkg > /dev/null; \
	/tmp/klang_api > /dev/null & api=$$!; \
	sleep 1; \
	"$$js" tests/fullstack_test.js "$$PWD/.klang/app/app.js"; rc=$$?; \
	kill $$api 2>/dev/null; \
	exit $$rc

# Every file here must be rejected, with a useful message.
.PHONY: test-errors
test-errors: bin/klangc
	@set -e; for f in tests/errors/*.kkg; do \
		printf '%-34s ' "$$(basename $$f)"; \
		if ./bin/klangc "$$f" -o /tmp/klang_err.c >/tmp/klang_err.txt 2>&1; then \
			echo "FAIL (compiled, but should not have)"; exit 1; \
		else \
			sed 's/^/  /' /tmp/klang_err.txt | head -1; \
		fi; \
	done

.PHONY: check
check: test test-gc test-new test-errors

.PHONY: clean
clean:
	rm -rf bin .klang
