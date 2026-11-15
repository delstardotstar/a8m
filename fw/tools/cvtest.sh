#!/bin/sh
# Builds and runs tools/cvtest.c on the (AArch64) host.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
work=$(cd "$here/.." && pwd)/tmp/cvtest
mkdir -p "$work"
cc -std=c11 -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/src" \
  "$here/tools/cvtest.c" "$here/src/cvframe.c" -o "$work/cvtest"
"$work/cvtest"
