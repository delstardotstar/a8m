#!/bin/sh
# Builds and runs tools/ymltest.c on the host with ASan/UBSan.
# usage: tools/ymltest.sh [STOCKDIR]   (default ../tmp/stock-folders)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)
stock=${1:-$root/tmp/stock-folders}
work=$root/tmp/yml
mkdir -p "$work/prs" "$work/obj"
inc="-I$here/src -I$here/tallent -I$here/mdkit/include"
for d in "$here"/mdkit/src/*/*; do inc="$inc -I$d"; done
mnmd="-I$here/mnmd/include -I$here/mnmd/voices -I$here/mdkit/include -I$here/src"
for f in "$here"/mnmd/src/*.cpp; do
  c++ -std=c++11 -O1 -fwrapv -fno-exceptions -fno-rtti -w $mnmd -c "$f" -o "$work/mnmd-$(basename "$f" .cpp).o"
done
cc -std=c99 -O1 -w $inc "$here/tools/presets.c" "$here/src/yml.c" "$here/src/r80law.c" "$here/src/r80_programs.c" "$here/src/r70law.c" "$here/src/r70_programs.c" "$here/tallent/eq_math.c" "$here/src/voices.c" "$here/src/sine_tables.c" \
  "$here"/mdkit/src/*/*/*.c "$work"/mnmd-*.o -lm -o "$work/presets"
"$work/presets" "$work/prs" >/dev/null
cc -std=c99 -O1 -w $inc -c "$here/src/voices.c" "$here/src/sine_tables.c" "$here"/mdkit/src/*/*/*.c
mv ./*.o "$work/obj/"
cc -std=c11 -O1 -g -Wall -Wextra -Wno-deprecated-declarations -Wno-gnu-variable-sized-type-not-at-end -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  $inc "$here/tools/ymltest.c" "$here/src/yml.c" "$here/src/r80law.c" "$here/src/r80_programs.c" "$here/src/r70law.c" "$here/src/r70_programs.c" "$here/tallent/eq_math.c" "$here/src/preset.c" "$here/src/wav.c" "$here/src/ops.c" "$here/src/ssetup.c" "$work"/obj/*.o "$work"/mnmd-*.o -lm -o "$work/ymltest"
exact=$root/tmp/samp2/probe-saved.yml
[ -f "$exact" ] && export YML_EXACT="$exact"
"$work/ymltest" "$work/prs" "$stock"/saved-prst001.yml "$stock"/src/prst00*.yml "$root/tmp/prst001.yml" "$work"/prs/prst*.yml
