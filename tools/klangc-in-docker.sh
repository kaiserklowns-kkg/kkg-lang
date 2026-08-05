#!/bin/sh
# The container half of tools/klangc.cmd (and tools/klangc, on a Unix host).
#
# Builds the compiler if its source has changed since the last time, then runs it
# with whatever arguments were passed. Kept in its own file because the alternative
# is a shell script embedded in a batch string, quoted twice.

set -e

if [ ! -x /w/bin/klangc-linux ] || [ /w/src/klangc.c -nt /w/bin/klangc-linux ]; then
    echo "klangc: building the compiler (once, about ten seconds)..." >&2
    mkdir -p /w/bin
    gcc -std=c99 -O2 -o /w/bin/klangc-linux /w/src/klangc.c -lm
fi

exec /w/bin/klangc-linux "$@"
