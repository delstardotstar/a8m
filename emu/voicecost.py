"""Estimate Cortex-A8 cycles per voice (and where they go) without hardware.

usage: uv run python -I emu/voicecost.py [VOICE...] [--top N] [--jobs N] [--lines FUNCTION] [--x8]

Also checks each voice's raw output on the firmware against the untouched
upstream mdkit (ref/mdkit), or for MNMD voices upstream ref/OP-XY-MNMD through emu/mnmdref.cpp,
built natively here (same controls, trigger and 32-frame renders): reports the
largest difference relative to the voice's peak.

Builds fw with PROFILE_VOICE=n (one second of that voice through the engine,
see fw/src/main.c), runs it in our QEMU machine under the hotblocks plugin,
and weights every executed instruction with rough Cortex-A8 costs (VFPLite is
not pipelined; VFP<->core transfers stall). Prints us per 667 us block and the
most expensive functions. VOICE is a voices[] index (1-35), or 99 for 8
looping sample voices (98: with audio-rate pitch/level CV routes; 97: phase mod, aliasing,
smoothing, crossfades; 96 / 95: the sample input's decimators and capture at 192 / 48 kHz;
no output check); default all voices. --x8: the voice on all 8 channels at once.
Hardware run 16 measured these within ~20% of the bench (e.g. TRX-BD 66 us).
"""
import bisect
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fw = os.path.join(root, 'fw')
qemu = os.path.join(root, 'ref/qemu/build/qemu-system-arm')
plugin = os.path.join(root, 'ref/qemu/build/contrib/plugins/libhotblocks.dylib')
BLOCKS = 1500                   # engine blocks rendered (1 s)
MHZ = 1000


def cost(mn, ops):
    """Cycles for one instruction (rough Cortex-A8)."""
    m = mn.split('.')[0]
    if m.startswith('v'):
        if m in ('vldr', 'vstr', 'vpush', 'vpop', 'vldm', 'vstmia', 'vstmdb', 'vldmia', 'vldmdb'):
            return 3
        if m in ('vmrs', 'vmsr'):
            return 20
        if m == 'vmov':
            # core <-> VFP transfers stall the pipeline
            return 20 if re.search(r'\br\d+\b|\bip\b|\blr\b', ops) else 3
        if m in ('vdiv', 'vsqrt'):
            return 30 if '.f64' in mn else 20
        if not re.search(r'\bq\d+', ops) and ('.f64' in mn or re.search(r'\bs\d+\b', ops)):
            return 9                    # VFPLite arithmetic, compares, converts (s or .f64 registers)
        return 2                        # NEON (d/q registers)
    if m in ('mul', 'mla', 'mls', 'umull', 'smull', 'umlal', 'smlal', 'smulbb', 'smulwb',
             'smlabb', 'smulwt', 'smultt', 'smmul', 'smmla'):
        return 3
    if m in ('sdiv', 'udiv'):
        return 20
    if m.startswith('ldr') or m.startswith('ldm') or m == 'pop':
        return 2
    if m.startswith('b') and m not in ('bic', 'bics', 'bfi', 'bfc'):
        return 2
    return 1


def disasm(elf):
    out = subprocess.run(['arm-none-eabi-objdump', '-d', '--no-show-raw-insn', '-m', 'armv7', elf],
                         capture_output=True, text=True, check=True).stdout
    ins = {}
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
        if m:
            ins[int(m.group(1), 16)] = cost(m.group(2), m.group(3))
    return ins


def symbols(elf):
    out = subprocess.run(['arm-none-eabi-nm', '-n', '-S', '-C', '--defined-only', elf],
                         capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split(None, 3)
        if len(p) == 4 and p[2] in 'tTW':
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    return syms


NATIVE = r"""
#include <stdio.h>
#include <stdlib.h>
#include "mdkit.h"
extern const mdkit_voice SYM;
int main(void) {
    const mdkit_voice *v = &SYM;
    void *st = calloc(1, v->state_bytes);
    float buf[32];
    v->init(st);
    for (unsigned i = 0; i < v->control_count; i++)
        v->set_control(st, i, v->control_defaults ? v->control_defaults[i] : 64);
    v->trigger(st);
    for (int n = 0; n < 22050; n += 32) {
        v->render(st, buf, 32);
        fwrite(buf, 4, 32, stdout);
    }
    return 0;
}
"""


def native(voice, work):
    """The voice from the untouched upstream mdkit (ref/mdkit), so local
    changes in fw/mdkit are checked against the original, not themselves."""
    src = open(os.path.join(fw, 'src', 'voices.c')).read()
    sym = re.findall(r'&(m(?:dkit|nmd)_voice_\w+)', src)[voice - 1]
    if sym.startswith('mnmd_'):
        return native_mnmd(sym[len('mnmd_voice_'):], src, work)
    up = os.path.join(root, 'ref', 'mdkit')
    d = os.path.join(up, 'src', *sym[len('mdkit_voice_'):].split('_', 1))
    c = os.path.join(work, 'native.c')
    open(c, 'w').write(NATIVE.replace('SYM', sym))
    exe = os.path.join(work, 'native')
    subprocess.run(['cc', '-std=c99', '-O2', '-ffp-contract=off', '-w', '-I' + os.path.join(up, 'include'), '-I' + d, c]
                   + [os.path.join(d, f) for f in os.listdir(d) if f.endswith('.c')] + ['-lm', '-o', exe], check=True)
    raw = subprocess.run([exe], capture_output=True, check=True).stdout
    return list(struct.unpack(f'<{len(raw) // 4}f', raw))


def native_mnmd(slug, src, work):
    params = re.search(r'p_%s\[\] = \{(.*?)\};' % slug, src, re.S).group(1)
    defs = re.findall(r'[PC]\("[^"]*", (\d+),', params)
    exe = os.path.join(work, 'mnmdref')
    ref = os.path.join(root, 'ref')
    subprocess.run(['c++', '-std=c++11', '-O2', '-fwrapv', '-w', '-I' + os.path.join(ref, 'open-xy', 'sdk', 'include'),
                    '-I' + os.path.join(ref, 'OP-XY-MNMD', 'lib'), '-I' + os.path.join(ref, 'OP-XY-MNMD', 'voices'),
                    '-I' + os.path.join(fw, 'mdkit', 'include'), os.path.join(root, 'emu', 'mnmdref.cpp'), '-o', exe],
                   check=True)
    raw = subprocess.run([exe, slug] + defs, capture_output=True, check=True).stdout
    return list(struct.unpack(f'<{len(raw) // 4}f', raw))


def check(voice, work):
    text = open(os.path.join(work, 'uart.log')).read()
    body = text.split('SAMPLES0', 1)[1].split('END', 1)[0]
    fw_out = [struct.unpack('<f', bytes.fromhex(h)[::-1])[0] for h in body.split()]
    ref = native(voice, work)
    peak = max(abs(x) for x in ref) or 1.0
    err = max(abs(a - b) for a, b in zip(fw_out, ref))
    return err / peak


X8 = '--x8' in sys.argv


def run(voice, jobs_dir, check_build=False):
    x8 = X8 and not check_build and voice
    build = f'build-{"chk" if check_build else "prof"}{voice}{"x8" if x8 else ""}'
    subprocess.run(['make', '-s', '-C', fw, f'BUILD={build}', f'PROFILE_VOICE={voice}', '-j4']
                   + (['PROFILE_CHECK=1'] if check_build else []) + (['EXTRA=-DPROFILE_ALL8'] if x8 else []),
                   check=True, capture_output=True)
    elf = os.path.join(fw, build, 'fw.elf')
    work = tempfile.mkdtemp(dir=jobs_dir)
    app = os.path.join(work, 'app.bin')
    with open(os.path.join(fw, build, 'app'), 'rb') as f:
        data = bytearray(f.read()[8:])
    data[0x34:0x38] = (1).to_bytes(4, 'little')
    open(app, 'wb').write(data)
    uart, log, sock = (os.path.join(work, n) for n in ('uart.log', 'plugin.log', 'mon.sock'))
    env = dict(os.environ, A8_AUDIO_DIR=work, A8_TIMESCALE='1')
    p = subprocess.Popen([qemu, '-M', 'assimil8or', '-display', 'none', '-serial', f'file:{uart}',
                          '-monitor', f'unix:{sock},server,nowait',
                          '-device', f'loader,file={app},addr=0x80000000,force-raw=on',
                          '-device', 'loader,addr=0x80000000,cpu-num=0',
                          ] + ([] if check_build else ['-plugin', f'{plugin},limit=0', '-d', 'plugin', '-D', log]),
                         env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(1200):
            time.sleep(0.5)
            if os.path.exists(uart) and b'DONE' in open(uart, 'rb').read():
                break
        else:
            raise RuntimeError(f'voice {voice}: no DONE')
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock)
        s.sendall(b'quit\n')
        p.wait(timeout=60)
    finally:
        if p.poll() is None:
            p.kill()
    if check_build:
        return check(voice, work)
    blocks = []
    for line in open(log):
        m = re.match(r'0x([0-9a-f]+), (\d+), (\d+), (\d+)', line)
        if m:
            blocks.append((int(m.group(1), 16), int(m.group(3)), int(m.group(4))))
    rel = run(voice, jobs_dir, True) if 0 < voice < 90 else 0.0
    return voice, elf, blocks, rel


def costs(elf, blocks):
    ins, syms = disasm(elf), symbols(elf)
    starts = [s[0] for s in syms]
    per_fn = {}
    for pc, n, execs in blocks:
        c = sum(ins.get(pc + 4 * i, 1) for i in range(n)) * execs
        i = bisect.bisect_right(starts, pc) - 1
        # assembly labels (start.S) have no size: up to the next symbol
        end = syms[i][0] + syms[i][1] if i >= 0 and syms[i][1] else (starts[i + 1] if 0 <= i < len(starts) - 1 else 0)
        name = syms[i][2] if i >= 0 and pc < end else hex(pc)
        per_fn[name] = per_fn.get(name, 0) + c
    return per_fn


def lines(elf, blocks, fn, top):
    """Costliest source lines inside function fn."""
    ins, syms = disasm(elf), symbols(elf)
    lo = [(a, a + sz) for a, sz, n in syms if n == fn]
    if not lo:
        return
    a0, a1 = lo[0]
    per_pc = {}
    for pc, n, execs in blocks:
        for i in range(n):
            a = pc + 4 * i
            if a0 <= a < a1:
                per_pc[a] = per_pc.get(a, 0) + ins.get(a, 1) * execs
    out = subprocess.run(['arm-none-eabi-addr2line', '-e', elf] + [hex(a) for a in per_pc],
                         capture_output=True, text=True).stdout.split('\n')
    per_line = {}
    for (a, c), loc in zip(per_pc.items(), out):
        loc = os.path.relpath(loc.split(' ')[0], fw) if loc.startswith('/') else loc
        per_line[loc] = per_line.get(loc, 0) + c
    total = sum(per_line.values()) or 1
    for loc, c in sorted(per_line.items(), key=lambda kv: -kv[1])[:top]:
        print(f'        {100 * c / total:5.1f}%  {c / BLOCKS / MHZ:6.1f} us  {loc}')


def report(voice, per_fn, base, top, rel):
    """Costs minus the no-machine run (boot, engine overhead, CV/mix)."""
    delta = {k: v - base.get(k, 0) for k, v in per_fn.items()}
    delta = {k: v for k, v in delta.items() if v > 0}
    # One-time work (voice init, table fills) runs in the main loop, not per block.
    once = {k: v for k, v in delta.items() if re.search(r'_init\b|_init_|_calc|_fill|^main$', k)}
    delta = {k: v for k, v in delta.items() if k not in once}
    total = sum(delta.values())
    us = total / BLOCKS / MHZ
    flag = 'OK' if rel < 1e-4 else 'DIFFERS'
    print(f'voice {voice:2}: ~{us:6.0f} us/block ({100 * us / 667:3.0f}%) over the no-machine baseline;'
          f' output vs native {rel:.1e} of peak {flag}; init ~{sum(once.values()) / MHZ / 1000:.0f} ms')
    for fn, c in sorted(delta.items(), key=lambda kv: -kv[1])[:top]:
        print(f'    {100 * c / total:5.1f}%  {c / BLOCKS / MHZ:6.1f} us  {fn[:90]}')


def main():
    args = sys.argv[1:]
    top = int(args[args.index('--top') + 1]) if '--top' in args else 6
    detail = args[args.index('--lines') + 1] if '--lines' in args else None
    jobs = int(args[args.index('--jobs') + 1]) if '--jobs' in args else 4
    voices = [int(a) for i, a in enumerate(args) if a.isdigit() and (i == 0 or args[i - 1] not in ('--top', '--jobs'))]
    count = len(re.findall(r'&(m(?:dkit|nmd)_voice_\w+)', open(os.path.join(fw, 'src', 'voices.c')).read()))
    voices = voices or list(range(1, count + 1))
    jobs_dir = os.path.join(root, 'tmp', 'voicecost')
    os.makedirs(jobs_dir, exist_ok=True)
    with ThreadPoolExecutor(jobs) as ex:
        base = None
        for voice, elf, blocks, rel in ex.map(lambda v: run(v, jobs_dir), [0] + voices):
            if base is None:
                base = costs(elf, blocks)
                print(f'baseline (no machine): ~{sum(base.values()) / BLOCKS / MHZ:.0f} us/block incl. boot')
                continue
            report(voice, costs(elf, blocks), base, top, rel)
            if detail:
                lines(elf, blocks, detail, 12)
            sys.stdout.flush()


if __name__ == '__main__':
    main()
