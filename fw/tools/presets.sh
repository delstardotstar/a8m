#!/bin/sh
# Builds tools/presets.c (or $SRC) natively against the vendored mdkit and MNMD
# voices and writes the presets to OUTDIR (e.g. "<card>/MACHINES DEMO").
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: tools/presets.sh OUTDIR}
tmp=$(mktemp -d)
inc="-I$here/src -I$here/tallent -I$here/mdkit/include"
for f in "$here"/mnmd/src/*.cpp; do
  c++ -std=c++11 -O1 -w -fwrapv -fno-exceptions -fno-rtti -I"$here/mnmd/include" -I"$here/mnmd/voices" \
    $inc -c "$f" -o "$tmp/$(basename "$f" .cpp).o"
done
cc -std=c99 -O1 -w $inc $(for d in "$here"/mdkit/src/*/*; do printf -- "-I%s " "$d"; done) \
  "${SRC:-$here/tools/presets.c}" "$here/src/yml.c" "$here/src/r80law.c" "$here/src/r80_programs.c" \
  "$here/src/r70law.c" "$here/src/r70_programs.c" "$here/tallent/eq_math.c" "$here/src/voices.c" \
  "$here/src/sine_tables.c" "$here"/mdkit/src/*/*/*.c "$tmp"/*.o -lc++ -lm -o "$tmp/presets"
mkdir -p "$out"
"$tmp/presets" "$out"
