#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/nb_core_tests"
cc -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
    -o "$OUT" "$ROOT/tests/test_core.c" "$ROOT/src/helper/nb_core.c"
"$OUT"
