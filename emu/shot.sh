#!/bin/sh
# Boots for A8_WAIT seconds (default 8) and saves the OLED to tmp/oled.png.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
sock="$root/tmp/mon.sock"
rm -f "$sock" "$root/tmp/oled.png"
"$root/emu/run.sh" -monitor unix:"$sock",server,nowait "$@" &
pid=$!
sleep "${A8_WAIT:-8}"
printf 'screendump %s -f png\n' "$root/tmp/oled.png" | nc -U -w2 "$sock" >/dev/null
kill $pid 2>/dev/null; wait $pid 2>/dev/null
ls -l "$root/tmp/oled.png"
