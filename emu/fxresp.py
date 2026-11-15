"""EQ and filter frequency response against the graphs' curves.

usage: uv run python -I emu/fxresp.py [TOL_DB]

Builds fw with -DFX_TEST (src/fxtest.c) into tmp/fxresp, runs it headless
and compares each case's measured gain with its curve (eq_curve or
filter_curve) at the curve's 31 frequencies, 20 Hz to 20 kHz. Points
where the curve is below -60 dB (fixed-point floor) or at the EQ graph's
+-21 dB clamp are left out.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voicecost as vc

tol = float(sys.argv[1]) if len(sys.argv) > 1 else 0.5
build = os.path.join(vc.root, 'tmp', 'fxresp')
subprocess.run(['make', '-s', '-C', vc.fw, f'BUILD={build}', 'EXTRA=-DFX_TEST', '-j8'], check=True,
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
    for _ in range(2400):
        time.sleep(0.5)
        if os.path.exists(uart) and b'DONE' in open(uart, 'rb').read():
            break
    else:
        raise RuntimeError('no DONE')
finally:
    p.kill()
    p.wait()
cases = {}
for line in open(uart, errors='replace'):
    t = line.split()
    if len(t) >= 5 and t[0] == 'R':
        k = int(t[4]) / 100
        if k > -60 and not (len(t) == 6 and abs(k) >= 20.9):
            cases.setdefault(int(t[1]), []).append((int(t[2]), int(t[3]) / 100, k))
bad = 0
for c, pts in sorted(cases.items()):
    used = pts
    worst = max(used, key=lambda x: abs(x[1] - x[2]))
    err = abs(worst[1] - worst[2])
    bad += err > tol
    print(f'case {c:2d}: {len(used):2d} points, worst {err:5.2f} dB at point {worst[0]} '
          f'(measured {worst[1]:+.2f}, curve {worst[2]:+.2f}){"  FAIL" if err > tol else ""}')
print('FAIL' if bad else 'PASS')
sys.exit(1 if bad else 0)
