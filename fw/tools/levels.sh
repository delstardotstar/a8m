#!/bin/sh
# Builds tools/levels.c on the host and measures every machine voice's
# raw output peaks (worst case and at defaults) and its level after the
# trim and clipper (or the voices given), in parallel; see tools/levels.c.
# usage: tools/levels.sh [VOICE...]   (env RANDOM_CASES, default 2000; SEED)
# Prints: id name worst_peak case default_peak default_rms p50 p95
#         rule_gain gain out_peak_db out_rms_db worst_in_db
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)
work=$root/tmp/levels
mkdir -p "$work"
inc="-I$here/src -I$here/mdkit/include"
for d in "$here"/mdkit/src/*/*; do inc="$inc -I$d"; done
mnmd="-I$here/mnmd/include -I$here/mnmd/voices -I$here/mdkit/include -I$here/src"
for f in "$here"/mnmd/src/*.cpp; do
  c++ -std=c++11 -O2 -fwrapv -fno-exceptions -fno-rtti -w $mnmd -c "$f" -o "$work/mnmd-$(basename "$f" .cpp).o"
done
cc -std=c99 -O2 -ffp-contract=off -w $inc "$here/tools/levels.c" "$here/src/voices.c" "$here/src/sine_tables.c" \
  "$here"/mdkit/src/*/*/*.c "$work"/mnmd-*.o -lc++ -lm -o "$work/levels"
ids=${*:-$(seq 1 43)}
for v in $ids; do echo "$v"; done | xargs -P "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" -I{} \
  "$work/levels" {} "${RANDOM_CASES:-2000}" "${SEED:-1}" | sort -n
