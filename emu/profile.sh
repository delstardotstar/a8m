#!/bin/sh
# Samples the PC N times (default 40) after a warm-up and prints a histogram.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
sock="$root/tmp/mon.sock"
rm -f "$sock"
"$root/emu/run.sh" -monitor unix:"$sock",server,nowait "$@" &
pid=$!
sleep "${A8_WAIT:-4}"
for i in $(seq "${A8_SAMPLES:-40}"); do
  printf 'info registers\n' | nc -U -w1 "$sock" | sed -n 's/.*R15=\([0-9a-f]*\).*/\1/p'
  sleep 0.1
done | sort | uniq -c | sort -rn
kill $pid 2>/dev/null; wait $pid 2>/dev/null
