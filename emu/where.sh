#!/bin/sh
# Runs the emulator for a few seconds, then prints the stuck PC with nearby code and the trace tail.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
sock="$root/tmp/mon.sock"
rm -f "$sock"
"$root/emu/run.sh" -monitor unix:"$sock",server,nowait "$@" &
pid=$!
sleep "${A8_WAIT:-4}"
regs=$(printf 'info registers\n' | nc -U -w1 "$sock")
kill $pid 2>/dev/null; wait $pid 2>/dev/null
echo "$regs" | grep -E '^R|PSR'
pc=$(echo "$regs" | sed -n 's/.*R15=\([0-9a-f]*\).*/\1/p')
uv run -q --with capstone python -I "$root/emu/dis.py" "$root/tmp/app.bin" "$(printf '0x%x:12' $((0x$pc - 24)))"
tail -n "${A8_TAIL:-15}" "$root/tmp/trace.log"
