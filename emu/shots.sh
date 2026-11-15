#!/bin/sh
# Saves OLED screenshots at the given times (seconds) into one strip: tmp/oled_seq.png.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
sock="$root/tmp/mon.sock"
rm -f "$sock" "$root"/tmp/oled_t*.png
"$root/emu/run.sh" -monitor unix:"$sock",server,nowait &
pid=$!
prev=0
for t in ${A8_TIMES:-2 4 6 9 12}; do
  sleep $(echo "$t - $prev" | bc)
  prev=$t
  printf 'screendump %s/tmp/oled_t%02d.png -f png\n' "$root" "$t" | nc -U -w1 "$sock" >/dev/null
done
kill $pid 2>/dev/null; wait $pid 2>/dev/null
uv run -q --with pillow python -I - "$root" <<'PY'
import glob, sys
from PIL import Image
fs = sorted(glob.glob(sys.argv[1] + '/tmp/oled_t*.png'))
ims = [Image.open(f).resize((256, 256)) for f in fs]
sheet = Image.new('RGB', (264 * len(ims) - 8, 256), (60, 60, 60))
for i, im in enumerate(ims):
    sheet.paste(im, (i * 264, 0))
sheet.save(sys.argv[1] + '/tmp/oled_seq.png')
PY
