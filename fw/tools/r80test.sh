#!/bin/sh
# Builds and runs tools/r80test.c on the host: the 80's laws against its
# program data, fw/src/r80.c against literal models of the spec's tank
# listings (tools/r80ref.py), every program's response, stability and edits.
# usage: tools/r80test.sh [PROGRAM]   (the description in ref/reverb-80,
#        or R80_SPEC=DIR; PROGRAM: one program, more output)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)
spec=${R80_SPEC:-$root/ref/reverb-80}
work=$root/tmp/r80test
mkdir -p "$work"
python3 -I "$here/tools/r80ref.py" "$spec/algorithms-and-programs.md" "$work/ref.c"
python3 -I "$here/tools/r80gen.py" --expect "$work/expect.c" "$spec" >/dev/null
python3 -I "$here/tools/r80steps.py" "$work/steps.h"
cmp -s "$work/steps.h" "$here/src/r80_steps.h" || { echo "r80test: fw/src/r80_steps.h is stale (tools/r80steps.py)"; exit 1; }
git -C "$root" diff --quiet -- fw/src/r80_programs.c fw/src/r80_gen.h ||
  echo "r80test: note: r80_programs.c / r80_gen.h differ from the generator's output"
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cc -std=c11 -O2 -g -Wall -Wextra $san -DR80_HOST -DREF_C="\"$work/ref.c\"" -DEXPECT_C="\"$work/expect.c\"" \
  -I"$here/src" "$here/tools/r80test.c" "$here/src/r80.c" "$here/src/r80law.c" "$here/src/r80_programs.c" \
  -lm -o "$work/r80test"
"$work/r80test" "$@"
