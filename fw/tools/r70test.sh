#!/bin/sh
# Builds and runs tools/r70test.c on the host: the 70's laws against its
# program data, fw/src/r70.c against literal models of the description's
# listings (tools/r70ref.py), RT60, stability, edits and the resamplers.
# usage: tools/r70test.sh [PROGRAM]   (the description in ref/reverb-70,
#        or R70_SPEC=DIR; PROGRAM: one program by number, more output)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)
spec=${R70_SPEC:-$root/ref/reverb-70}
work=$root/tmp/r70test
mkdir -p "$work"
python3 -I "$here/tools/r70ref.py" "$spec/reverb-programs.json" "$work/ref.c"
python3 -I "$here/tools/r70gen.py" --expect "$work/expect.c" "$spec" >/dev/null
git -C "$root" diff --quiet -- fw/src/r70_programs.c fw/src/r70_gen.h fw/src/r70_nets.h ||
  echo "r70test: note: the generated files differ from the generator's output"
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cc -std=c11 -O2 -g -Wall -Wextra $san -DR70_HOST -DREF_C="\"$work/ref.c\"" -DEXPECT_C="\"$work/expect.c\"" \
  -I"$here/src" "$here/tools/r70test.c" "$here/src/r70.c" "$here/src/r70law.c" "$here/src/r70_programs.c" \
  -lm -o "$work/r70test"
"$work/r70test" "$@"
