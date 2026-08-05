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
check: test test-gc test-errors

.PHONY: clean
clean:
	rm -rf bin
