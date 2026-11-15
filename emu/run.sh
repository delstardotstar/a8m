#!/bin/sh
# Usage: emu/run.sh [extra qemu args]
# Runs $A8_APP (default stock/app-v304) from 0x80000000 (8-byte image header stripped),
# with the word MLO writes at 0x80000034 (1, seen on hardware).
# A8_TIMESCALE (default 4) slows all device clocks so the guest gets real-hardware CPU per emulated second.
# Output audio is recorded to tmp/mcasp{0,1}.raw (s32le; 8 and 2 channels at 48 kHz).
# A8_HWCHECK=0 drops the hardware-hang checks (emu/plugin/uncached.c, FIQ in the INTC).
# A8_HWCHECK_ARGS passes options to that plugin (e.g. halt=off,map=on).
# A8_QEMU picks another QEMU tree (default ref/qemu).
# Attaches tmp/sd0.img (SOM card, unit 0) and tmp/sd1.img (front card, unit 1) if present.
# A software reset (PRM_RSTCTRL; QEMU exits 4) boots again like power-on: emu/mlo.py loads
# /app from tmp/sd0.img as MLO would. A8_APP=card boots that way from the start;
# A8_RESET=0 ignores the reset. A WDT1 reset (emu/am335x-wdt.c, QEMU exits 6) boots again the
# same way, with PRM_RSTST's WDT1_RST bit set (A8_RSTST); A8_WDT=0 ignores it.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/tmp"
app="${A8_APP:-$root/stock/app-v304}"
if [ "$app" = card ]; then
  python3 -I "$root/emu/mlo.py" "$root/tmp/sd0.img" "$root/tmp/app.bin"
else
  tail -c +9 "$app" > "$root/tmp/app.bin"
  # MLO writes 1 at load address + 0x34 after copying (hardware run 8).
  printf '\001\000\000\000' | dd of="$root/tmp/app.bin" bs=1 seek=52 conv=notrunc 2>/dev/null
fi
for n in 1 0; do
  img="$root/tmp/sd$n.img"
  [ -f "$img" ] && set -- -drive if=sd,index=$n,format=raw,file="$img" "$@"
done
build="${A8_QEMU:-$root/ref/qemu}/build"
if [ "${A8_HWCHECK:-1}" != 0 ] && [ -f "$build/libuncached.dylib" ]; then
  set -- -plugin "$build/libuncached.dylib${A8_HWCHECK_ARGS:+,$A8_HWCHECK_ARGS}" "$@"
elif [ "${A8_HWCHECK:-1}" != 0 ]; then
  echo "run.sh: no $build/libuncached.dylib (rerun emu/setup.sh); uncached check off" >&2
fi
export A8_HWCHECK="${A8_HWCHECK:-1}"
export A8_AUDIO_DIR="$root/tmp"
export A8_TIMESCALE="${A8_TIMESCALE:-4}"
# Below 16 our firmware's main loop starves under emulation and WDT1 would
# reset it: ignore WDT1 there unless asked.
[ "$A8_TIMESCALE" -lt 16 ] && export A8_WDT="${A8_WDT:-0}"
export A8_RSTST=1
pid=
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; wait; exit 143' TERM INT HUP
while :; do
  "$build/qemu-system-arm" -M assimil8or -display "${A8_DISPLAY:-none}" -monitor none \
  -serial "file:$root/tmp/uart0.log" \
  -device loader,file="$root/tmp/app.bin",addr=0x80000000,force-raw=on \
  -device loader,addr=0x80000000,cpu-num=0 \
  -d unimp,guest_errors -D "$root/tmp/trace.log" "$@" &
  pid=$!
  rc=0
  wait "$pid" || rc=$?
  case $rc in
    4) A8_RSTST=1 ;;
    6) A8_RSTST=0x10 ;;
    *) exit "$rc" ;;
  esac
  python3 -I "$root/emu/mlo.py" "$root/tmp/sd0.img" "$root/tmp/app.bin" || exit 5
done
