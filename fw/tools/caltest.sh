#!/bin/sh
# Builds and runs tools/caltest.c (Calibrate's loopback signs) on the (AArch64) host.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
work=$(cd "$here/.." && pwd)/tmp/caltest
mkdir -p "$work"
cc -std=c11 -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/src" \
  "$here/tools/caltest.c" "$here/src/calmath.c" -o "$work/caltest"
"$work/caltest"
