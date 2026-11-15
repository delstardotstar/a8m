#!/bin/sh
# Wires emu/ into the QEMU tree (A8_QEMU, default ref/qemu) and builds
# qemu-system-arm and the emu/plugin/ TCG plugins.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
q="${A8_QEMU:-$root/ref/qemu}"

for f in "$root"/emu/*.c "$root"/emu/*.h; do ln -sf "$f" "$q/hw/arm/$(basename "$f")"; done
srcs=$(cd "$root/emu" && ls *.c | sed "s/.*/'&'/" | paste -sd, -)
sed -i '' '/CONFIG_ASSIMIL8OR/d' "$q/hw/arm/meson.build"
echo "arm_common_ss.add(when: 'CONFIG_ASSIMIL8OR', if_true: files($srcs))" >> "$q/hw/arm/meson.build"
git -C "$q" checkout -q -- hw/arm/Kconfig
printf '\nconfig ASSIMIL8OR\n    bool\n    default y\n    depends on TCG && ARM\n%s\n' \
  "$(printf '    select %s\n' UNIMP SDHCI SD SERIAL_MM)" >> "$q/hw/arm/Kconfig"

mkdir -p "$q/build"
cd "$q/build"
[ -f build.ninja ] || ../configure --target-list=arm-softmmu --disable-docs \
  --disable-werror --enable-cocoa --disable-sdl --disable-gtk
ninja qemu-system-arm
for f in "$root"/emu/plugin/*.c; do
  cc -shared -O2 -Wall -I../include/plugins $(pkg-config --cflags glib-2.0) \
    -undefined dynamic_lookup -o "lib$(basename "$f" .c).dylib" "$f"
done
