"""Worst single render block under emu/voicecost.py's cost model.

usage: uv run python -I emu/worst.py NAME PROFILE [make args...]
  e.g. emu/worst.py scene 93 "EXTRA=-DPROFILE_EVERY=50 -DPROFILE_PITCH=1200"

Builds fw with PROFILE_VOICE=PROFILE into tmp/worst/NAME and runs it under
emu/plugin/blockcost.c (built by emu/setup.sh), which costs every
engine_render call on its own (main.c's profile_mark). Prints the worst
block overall, the worst trigger block, the block after it and the worst
other block (all after the first PROFILE_EVERY blocks: the first trigger is
cold), and the costliest functions of the worst warm block (NFN, default
14; COLD=1 includes the cold blocks; HW=1 adds emu/plugin/uncached.c;
MEANFN=N the N costliest functions over all warm blocks; FNLINES=fn,... splits
those functions by source line).
PROFILE 92 with CARD=<image> plays the card's last folder and preset (its
A8M-PREFS.YML), all 8 channels hit together.
"""
import bisect
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voicecost as vc

name, profile, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
every = int((re.findall(r'PROFILE_EVERY=(\d+)', ' '.join(extra)) or [375])[0])
ab = int((re.findall(r'AUDIO_BLOCK=(\d+)', ' '.join(extra))
          or re.findall(r'define AUDIO_BLOCK\s+(\d+)', open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                     '..', 'fw', 'src', 'audio.h')).read()))[0])
per = 32 // ab  # blocks per 32 frames; PROFILE_EVERY counts 32-frame spans
every *= per
qb = os.path.join(vc.root, 'ref', 'qemu', 'build')
build = os.path.join(vc.root, 'tmp', 'worst', name)
subprocess.run(['make', '-s', '-C', vc.fw, f'BUILD={build}', f'PROFILE_VOICE={profile}', '-j8'] + extra,
               check=True, capture_output=True)
elf = os.path.join(build, 'fw.elf')
ins, syms = vc.disasm(elf), vc.symbols(elf)
starts = [s[0] for s in syms]
lfn = os.environ.get('FNLINES')   # FNLINES=fn,...: those functions' costs by source line (as pseudo-functions)
fidx = {a: max(bisect.bisect_right(starts, a) - 1, 0) for a in ins}
if lfn:
    pick = [a for a, j in fidx.items() if syms[j][2] in lfn.split(',')]
    out = subprocess.run(['arm-none-eabi-addr2line', '-e', elf] + [hex(a) for a in pick],
                         capture_output=True, text=True).stdout.splitlines()
    for a, src in zip(pick, out):
        name = syms[fidx[a]][2] + ':' + src.split('/')[-1].split()[0]
        syms.append((a, 0, name))
        fidx[a] = len(syms) - 1
    names = {}
    for j in range(len(starts), len(syms)):
        names.setdefault(syms[j][2], j)
    for a in pick:
        fidx[a] = names[syms[fidx[a]][2]]
with open(os.path.join(build, 'costs.txt'), 'w') as f:
    for a, c in ins.items():
        f.write(f'{a:x} {c} {fidx[a]}\n')
mark = next(s[0] for s in syms if s[2] == 'profile_mark')
work = os.path.join(build, 'run')
os.makedirs(work, exist_ok=True)
data = bytearray(open(os.path.join(build, 'app'), 'rb').read()[8:])
data[0x34:0x38] = (1).to_bytes(4, 'little')
open(os.path.join(work, 'app.bin'), 'wb').write(data)
uart, sock, out = (os.path.join(work, n) for n in ('uart.log', 'mon.sock', 'windows.txt'))
for p in (uart, sock, out):
    if os.path.exists(p):
        os.remove(p)
p = subprocess.Popen([vc.qemu, '-M', 'assimil8or', '-display', 'none', '-serial', f'file:{uart}',
                      '-monitor', 'unix:mon.sock,server,nowait',
                      '-device', f'loader,file={work}/app.bin,addr=0x80000000,force-raw=on',
                      '-device', 'loader,addr=0x80000000,cpu-num=0']
                     + (['-plugin', os.path.join(qb, 'libuncached.dylib')] if os.environ.get('HW') else [])
                     + (['-drive', f'if=sd,index=1,format=raw,file={os.path.abspath(os.environ["CARD"])}']
                        if os.environ.get('CARD') else [])
                     + ['-plugin', f'{qb}/libblockcost.dylib,costs={build}/costs.txt,mark={mark:x},out={out}'
                                     + ('' if os.environ.get('COLD') else f',from={every}')],
                     env=dict(os.environ, A8_AUDIO_DIR=work, A8_TIMESCALE='1'), cwd=work,
                     stdout=open(os.path.join(work, 'qemu.log'), 'w'), stderr=subprocess.STDOUT)
try:
    for _ in range(2400):
        time.sleep(0.5)
        if os.path.exists(uart) and b'DONE' in open(uart, 'rb').read():
            break
    else:
        raise RuntimeError('no DONE')
    s = socket.socket(socket.AF_UNIX)
    # Relative: deep worktree paths pass the 104-byte socket path limit.
    os.chdir(work)
    s.connect('mon.sock')
    s.sendall(b'quit\n')
    p.wait(timeout=60)
finally:
    if p.poll() is None:
        p.kill()
wins, tops, sums = [], [], {}
for line in open(out):
    t = line.split()
    if t[0] == 'W':
        wins.append((int(t[1]), int(t[2])))
    elif t[0] == 'T':
        tops.append([int(t[1]), int(t[2]), {}])
    elif t[0] == 'F':
        tops[-1][2][int(t[1])] = int(t[2])
    elif t[0] == 'S':
        sums[int(t[1])] = int(t[2])


def us(c):
    return c / vc.MHZ * per     # per 32 frames (667 us): a block's cost x 32 / AUDIO_BLOCK


warm = [(b % every, c) for b, c in wins if b >= every]
trig = max(c for k, c in warm if k == 0)
after = max((c for k, c in warm if k == 1), default=0)
steady = [c for k, c in warm if k > 1]
print(f'{name}: {len(wins)} blocks, mean {us(sum(c for _, c in wins) / len(wins)):.0f} us, worst {us(max(c for _, c in wins)):.0f};'
      f' warm: trigger block {us(trig):.0f}, next {us(after):.0f}, others {us(max(steady)):.0f}'
      f' (mean {us(sum(steady) / len(steady)):.0f})')
if per > 1:
    spans = [sum(c for _, c in wins[i:i + per]) for i in range(0, len(wins) - per + 1, per)]
    print(f'  AUDIO_BLOCK {ab}: figures are x{per} (per 32 frames); worst 32-frame span'
          f' {us(max(spans[every // per:])) / per:.0f}, mean {us(sum(spans) / len(spans)) / per:.0f}')
tops = sorted((t for t in tops if t[0] >= every or os.environ.get('COLD')), key=lambda t: -t[1])
for b, total, fns in tops[:1]:
    print(f'  block {b} ({b % every} after a trigger): {us(total):.0f} us')
    for j, c in sorted(fns.items(), key=lambda kv: -kv[1])[:int(os.environ.get('NFN', 14))]:
        print(f'    {us(c):7.1f} us  {syms[j][2] if j < len(syms) else "?"}')
if sums and os.environ.get('MEANFN'):
    n = sum(1 for b, _ in wins if b >= (0 if os.environ.get('COLD') else every))
    print(f'  mean per 32 frames over {n} blocks:')
    for j, c in sorted(sums.items(), key=lambda kv: -kv[1])[:int(os.environ['MEANFN'])]:
        print(f'    {us(c / n):7.1f} us  {syms[j][2] if j < len(syms) else "?"}')
