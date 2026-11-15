#!/bin/sh
# Builds and runs tools/graintest.c on the host: fw/tallent/grains.c beside
# upstream's grain engine (ext/grains.c lines 3-499, untouched).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)
work=$root/tmp/graintest
up=$root/ref/assimil8or-extended-firmware/ext/grains.c
mkdir -p "$work"
{ cat "$here/tools/grainshim.h"; sed -n '3,499p' "$up"; cat "$here/tools/grainup.c"; } > "$work/up.c"
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cc -std=c11 -O1 -w $san -fno-sanitize=shift-base -c "$work/up.c" -o "$work/up.o"
cc -std=c11 -O1 -Wall -Wextra $san -I"$here/src" -I"$here/tallent" \
  "$here/tools/graintest.c" "$here/tallent/grains.c" "$work/up.o" -o "$work/graintest"
"$work/graintest"
