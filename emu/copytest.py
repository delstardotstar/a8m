"""COPY CHANNEL carries a channel's effects (fw/src/ui.c ui_copy_selftest).

usage: uv run python -I emu/copytest.py

Builds fw with -DCOPY_TEST into tmp/copytest, runs it headless (no card:
the live INIT preset, unsaved) and prints the checks.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voicecost as vc

build = os.path.join(vc.root, 'tmp', 'copytest')
subprocess.run(['make', '-s', '-C', vc.fw, f'BUILD={build}', 'EXTRA=-DCOPY_TEST', '-j8'], check=True,
               capture_output=True)
data = bytearray(open(os.path.join(build, 'app'), 'rb').read()[8:])
data[0x34:0x38] = (1).to_bytes(4, 'little')
open(os.path.join(build, 'app.bin'), 'wb').write(data)
uart = os.path.join(build, 'uart.log')
if os.path.exists(uart):
    os.remove(uart)
p = subprocess.Popen([vc.qemu, '-M', 'assimil8or', '-display', 'none', '-serial', f'file:{uart}',
                      '-device', f'loader,file={build}/app.bin,addr=0x80000000,force-raw=on',
                      '-device', 'loader,addr=0x80000000,cpu-num=0'],
                     env=dict(os.environ, A8_TIMESCALE='1'), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(600):
        time.sleep(0.5)
        if os.path.exists(uart) and b'DONE' in open(uart, 'rb').read():
            break
    else:
        raise RuntimeError('no DONE')
finally:
    p.kill()
    p.wait()
out = open(uart, errors='replace').read()
print(out.replace('DONE\n', '').strip())
sys.exit(0 if 'COPY PASS' in out else 1)
