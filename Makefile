CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
EXAMPLES := hello basics phase1

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
		$(CC) $(CFLAGS) -o /tmp/klang_$$f /tmp/klang_$$f.c; \
		/tmp/klang_$$f; \
	done

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
check: test test-errors

.PHONY: clean
clean:
	rm -rf bin
