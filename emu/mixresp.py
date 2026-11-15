"""Compressors, reverb and delay measured against Tallent's model.

usage: uv run python -I emu/mixresp.py

Builds fw with -DMIXFX_TEST (src/mixtest.c) into tmp/mixresp, runs it
headless and checks: the compressors' static gain (hard knee, slope 1 -
1/ratio, makeup), drive's small-signal gain, attack/release one-pole times
(ext/mix_fx.c time_coef), the reverb's decay time against Decay (combs set
to fall 60 dB in it) and the gated burst's length against Gate, and the
delay's echo times per note value at a tempo (halved past 1.37 s),
alternating sides, each repeat at Feedback. The 80's reverb (fw/src/r80.c)
through the same bus: its decay against the spec's literal model.
"""
import math
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voicecost as vc

build = os.path.join(vc.root, 'tmp', 'mixresp')
subprocess.run(['make', '-s', '-C', vc.fw, f'BUILD={build}', 'EXTRA=-DMIXFX_TEST', '-j8'], check=True,
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
    for _ in range(4800):
        time.sleep(0.5)
        if os.path.exists(uart) and b'DONE' in open(uart, 'rb').read():
            break
    else:
        raise RuntimeError('no DONE')
finally:
    p.kill()
    p.wait()

slope = {2: 0.5, 4: 0.75, 8: 1.0}
attack_ms = [0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100]
release_ms = [20, 50, 100, 200, 500, 1000, 2000]
division = [12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 192]
delays = {0: (3, 120, 50), 1: (1, 100, 50), 2: (10, 40, 80), 3: (8, 133, 100)}
bad = 0


def check(ok, text):
    global bad
    bad += not ok
    print(text + ('' if ok else '  FAIL'))


echoes = {}
for line in open(uart, errors='replace'):
    t = line.split()
    if not t:
        continue
    v = [int(x) for x in t[1:]]
    if t[0] == 'S':
        case, ratio, level, got = v
        makeup = 6 if case == 0 else 0
        want = makeup - max(0, level + 20) * slope[ratio]
        check(abs(got / 100 - want) <= 0.1, f'{"mix" if case else "ch1"} comp {ratio}: {level:+3d} dB in, '
              f'gain {got / 100:+6.2f} dB (model {want:+6.2f})')
    elif t[0] == 'D':
        amount, level, got = v
        # drive_build's curve: 256 steps of (1 + k) v / (1 + k v), read
        # interpolated (channels at 2^30, so the step is 2^-8 of full scale)
        k = 2 ** (5 * amount / 100) - 1
        x = 10 ** (level / 20)
        i = min(int(x * 256), 255)
        y = lambda v: (1 + k) * v / (1 + k * v)
        f = x * 256 - i
        want = 20 * math.log10((y(i / 256) + (y((i + 1) / 256) - y(i / 256)) * f) / x)
        check(abs(got / 100 - want) <= 0.15, f'drive {amount}: {level:+3d} dB in, gain {got / 100:+6.2f} dB '
              f'(model {want:+6.2f})')
    elif t[0] == 'T':
        rel, idx, got = v
        ms = (release_ms if rel else attack_ms)[idx]
        c = 100 / (100 + ms * 100 * 48)
        want = -1 / math.log(1 - c)
        check(got > 0 and abs(got / 100 - want) <= max(1.0, 0.02 * want),
              f'{"release" if rel else "attack"} {ms} ms: 63 % in {got / 100:.1f} frames (model {want:.1f})')
    elif t[0] == 'V':
        algo, decay, rt = v
        check(abs(rt / 1000 - decay / 10) <= 0.25 * decay / 10,
              f'reverb algo {algo + 1}: Decay {decay / 10:.1f} s, RT60 {rt / 1000:.2f} s (T30 x 2)')
    elif t[0] == 'W':
        # The 80's programs: the spec's literal model, T20 (fw/tools/r80test.sh).
        prog, _, rt = v
        want = {170: 2.08, 151: 0.50, 152: 0.41, 159: 1.59, 163: 1.58}[prog]
        check(abs(rt / 1000 - want) <= 0.15 * want, f'80 program {prog}: RT60 {rt / 1000:.2f} s (T20 x 3; literal model {want:.2f})')
    elif t[0] == 'X':
        # The 70's programs: the host engine's RT60 for the same one-sample
        # impulse (DECAY OPT makes it level dependent), T20 x 3.
        prog, _, rt = v
        want = {21: 4.62, 26: 0.42, 34: 1.77, 70: 1.86, 84: 1.41}[prog]
        check(abs(rt / 1000 - want) <= 0.15 * want, f'70 program {prog}: RT60 {rt / 1000:.2f} s (T20 x 3; host {want:.2f})')
    elif t[0] == 'G':
        algo, decay, end = v
        gate = decay * 5
        check(gate * 0.8 <= end <= gate * 1.4 + 40, f'gated algo {algo + 1}: Gate {gate} ms, energy -60 dB at {end} ms')
    elif t[0] == 'E':
        echoes.setdefault(v[0], []).append(v[1:])
for case, es in sorted(echoes.items()):
    d, bpm, fb = delays[case]
    frames = 48000 * 60 // bpm * division[d] // 48
    while frames > 65536 - 4:
        frames >>= 1
    frames = max(frames, 64)
    want_side = [0, 1, 0]
    for n, (side, at, amp) in enumerate(es):
        check(side == want_side[n] and abs(at - frames * (n + 1)) <= 2,
              f'delay {["1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1/1"][d]} at '
              f'{bpm} BPM: echo {n + 1} {"LR"[side]} at {at} (model {frames * (n + 1)})')
    for n in range(2):
        r = es[n + 1][2] / es[n][2]
        check(abs(r - fb / 100) <= 0.03, f'    echo {n + 2} / echo {n + 1} = {r:.3f} (Feedback {fb} %)')
print('FAIL' if bad else 'PASS')
sys.exit(1 if bad else 0)
