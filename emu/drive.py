"""Boot the emulator and run a key script against it.

usage: uv run --with pillow python -I emu/drive.py STEPS... [-- QEMU ARGS]
Steps: 'wait:S' (seconds), 'key:NAME[:MS]' (monitor sendkey, held MS),
'shot:NAME' (OLED to tmp/NAME.png), 'boot' (wait for the home screen),
'cmd:TEXT' (any monitor command, '_' for spaces).
Writes tmp/drive.png, a strip of all shots in order.
"""
import os
import subprocess
import sys
import time

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from monitor import Monitor

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = sys.argv[1:]
qemu_args = args[args.index('--') + 1:] if '--' in args else []
steps = args[:args.index('--')] if '--' in args else args
scale = float(os.environ.get('A8_TIMESCALE', '4'))
sock = f'{root}/tmp/mon.sock'
if os.path.exists(sock):
    os.unlink(sock)
p = subprocess.Popen([f'{root}/emu/run.sh', '-monitor', f'unix:{sock},server,nowait'] + qemu_args,
                     stderr=subprocess.DEVNULL)
shots = []
try:
    m = Monitor(sock)
    for st in steps:
        kind, _, arg = st.partition(':')
        if kind == 'boot':
            time.sleep(14 * scale)
        elif kind == 'wait':
            time.sleep(float(arg))
        elif kind == 'key':
            name, _, ms = arg.partition(':')
            ms = int(ms or 100)
            m.cmd(f'sendkey {name} {int(ms * scale)}')
            time.sleep((ms / 1000 + 0.35) * scale)
        elif kind == 'cmd':
            out = m.cmd(arg.replace('_', ' ')).decode(errors='replace')
            print('\n'.join(out.split('\r\n')[1:-1]), flush=True)
        elif kind == 'shot':
            f = f'{root}/tmp/{arg}.png'
            m.cmd(f'screendump {f} -f png')
            time.sleep(0.2)
            shots.append((arg, f))
finally:
    p.terminate()
    p.wait()
if shots:
    S = 200
    sheet = Image.new('RGB', (min(len(shots), 6) * (S + 6), ((len(shots) + 5) // 6) * (S + 20)), (50, 50, 50))
    d = ImageDraw.Draw(sheet)
    for i, (name, f) in enumerate(shots):
        x, y = (i % 6) * (S + 6), (i // 6) * (S + 20)
        sheet.paste(Image.open(f).resize((S, S)), (x, y + 18))
        d.text((x + 2, y + 2), name, fill=(255, 255, 0))
    sheet.save(f'{root}/tmp/drive.png')
