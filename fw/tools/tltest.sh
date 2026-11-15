#!/bin/sh
# Builds and runs tools/tltest.c on the host: fw/tallent/timelock.c and
# joins.c beside upstream's Time Lock engine (ext/timelock.c up to its
# foreground, and ext/joins.c, untouched).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)
work=$root/tmp/tltest
up=$root/ref/assimil8or-extended-firmware/ext
mkdir -p "$work/inc"
cp "$here/tools/tlshim.h" "$work/inc/ext.h"
: > "$work/inc/mix_fx.h"
# Mach-O wants "segment,section": the cold section is only a placement.
sed 's/section(".text.timelock_events")/section("__TEXT,__tl_events")/' "$up/timelock.h" > "$work/inc/timelock.h"
{ sed -n "1,/---- foreground ----/p" "$up/timelock.c"; echo "static u32 active;"; sed -n "/^void timelock_init(void)/,\$p" "$up/timelock.c"; cat "$here/tools/tlup.c"; } > "$work/up.c"
# With our one change (UPSTREAM): reads scaled to +-1, so the correlation
# sums of a loud loop stay finite.
sed 's|^    return v;$|    return v * (1.0f / 2147483648.0f);|' "$up/joins.c" > "$work/upjoins.c"
grep -q "return v \* (1.0f" "$work/upjoins.c"
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cc -std=c11 -O1 -w $san -fno-sanitize=shift-base -I"$work/inc" -c "$work/up.c" -o "$work/up.o"
cc -std=c11 -O1 -w $san -fno-sanitize=shift-base -I"$work/inc" -c "$work/upjoins.c" -o "$work/upjoins.o"
cc -std=c11 -O1 -Wall -Wextra $san -I"$here/src" -I"$here/tallent" \
  "$here/tools/tltest.c" "$here/tallent/timelock.c" "$here/tallent/joins.c" "$here/tallent/hits.c" \
  "$work/up.o" "$work/upjoins.o" -o "$work/tltest"
"$work/tltest"
