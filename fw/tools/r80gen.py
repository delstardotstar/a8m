"""Writes fw/src/r80_gen.h and fw/src/r80_programs.c: the 80's reverb
programs (Chamber, Plate, Concert Hall, Inverse, Chorus+Rvb) as their
stored parameters, modulators and patches, and the law tables, from the
written description (ref/reverb-80). Tables the spec gives only as the
unit's data are not read: our own laws replace them.

usage: python3 -I fw/tools/r80gen.py [--expect OUT.c] [DIR]
  DIR holds algorithms-and-programs.md and effect-programs.json
  (default ref/reverb-80). --expect also writes the program data's
  resulting values (tank k and A, input/output values, parameters in
  effect) for tools/r80test.c.
"""
import json
import math
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
expect = None
if args and args[0] == '--expect':
    expect = args[1]
    args = args[2:]
src = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'ref', 'reverb-80')
spec = open(os.path.join(src, 'algorithms-and-programs.md'), encoding='utf-8').read()
data = json.load(open(os.path.join(src, 'effect-programs.json'), encoding='utf-8'))

ALGOS = {'Chamber': 'R80_CHAMBER', 'Plate': 'R80_PLATE', 'Concert Hall': 'R80_HALL', 'Inverse': 'R80_INVERSE',
         'Chorus+Rvb': 'R80_CHORUS'}
TANK = {'5.1 Chamber': 0, '5.2 Plate': 1, '5.3 Concert Hall': 2, '5.4 Inverse': 3}

V = ['Voice%d' % j for j in range(1, 7)]
# (enum suffix, page, name); the order is the enum's.
PARAMS = [
    ('INLVL_R', 'Controls', 'InLvl R'), ('INLVL_L', 'Controls', 'InLvl L'), ('INPAN_R', 'Controls', 'InPan R'),
    ('INPAN_L', 'Controls', 'InPan L'), ('FXMIX', 'Controls', 'FX Mix'), ('FXADJUST', 'Controls', 'FX Adjust'),
    ('FXWIDTH', 'Controls', 'FX Width'), ('VOICEDIF', 'Controls', 'Voice Dif'), ('HIGHCUT', 'Controls', 'High Cut'),
    ('RTHC', 'Rvb Time', 'Rt HC'), ('LOWRT', 'Rvb Time', 'Low Rt'), ('XOVER', 'Rvb Time', 'Crossover'),
    ('MIDRT', 'Rvb Time', 'Mid Rt'), ('PREDELAY', 'Rvb Time', 'Pre Delay'), ('EKODLY_L', 'Rvb Time', 'EkoDly L'),
    ('EKODLY_R', 'Rvb Time', 'EkoDly R'), ('EKOFBK_L', 'Rvb Time', 'EkoFbk L'), ('EKOFBK_R', 'Rvb Time', 'EkoFbk R'),
    ('REFDLY_L', 'Rvb Time', 'RefDly L'), ('REFDLY_R', 'Rvb Time', 'RefDly R'), ('REFLVL_L', 'Rvb Time', 'RefLvl L'),
    ('REFLVL_R', 'Rvb Time', 'RefLvl R'), ('PSTDLY_L', 'Rvb Time', 'PstDly L'), ('PSTDLY_R', 'Rvb Time', 'PstDly R'),
    ('PSTMIX', 'Rvb Time', 'PstMix'), ('RGLDRESP', 'Rvb Time', 'GldResp'), ('RGLDRANGE', 'Rvb Time', 'GldRange'),
    ('LOWSLOPE', 'Rvb Time', 'Low Slope'), ('MIDSLOPE', 'Rvb Time', 'Mid Slope'),
    ('SIZE', 'RvbDesign', 'Size'), ('APSWEEP', 'RvbDesign', 'AP Sweep'), ('DIFFUSION', 'RvbDesign', 'Diffusion'),
    ('SPREAD', 'RvbDesign', 'Spread'), ('SHAPE', 'RvbDesign', 'Shape'), ('ATTACK', 'RvbDesign', 'Attack'),
    ('LINK', 'RvbDesign', 'Link'), ('RVBOUT', 'RvbDesign', 'Rvb Out'), ('RVBIN', 'RvbDesign', 'Rvb In'),
    ('RVBWIDTH', 'RvbDesign', 'Rvb Width'), ('DEF', 'RvbDesign', 'Def'), ('DEPTH', 'RvbDesign', 'Depth'),
    ('CHORUS', 'RvbDesign', 'Chorus'), ('DURATION', 'RvbDesign', 'Duration'),
    ('LVL_MASTER', 'Levels', 'Master')] + [('LVL_V%d' % j, 'Levels', V[j - 1]) for j in range(1, 7)] + [
    ('DLY_MASTER', 'DelayTime', 'Master')] + [('DLY_V%d' % j, 'DelayTime', V[j - 1]) for j in range(1, 7)] + [
    ('CLEAR', 'DelayTime', 'Clear'), ('VGLDRESP', 'DelayTime', 'GldResp'), ('VGLDRANGE', 'DelayTime', 'GldRange'),
    ('MSTDEPTH', 'Chorus', 'MstDepth')] + [('DEPTH_V%d' % j, 'Chorus', 'V%d Depth' % j) for j in range(1, 7)] + [
    ('MSTRATE', 'Chorus', 'MstRate')] + [('RATE_V%d' % j, 'Chorus', 'V%d Rate' % j) for j in range(1, 7)] + [
    ('FB_MASTER', 'Feedback', 'Master')] + [('FB_V%d' % j, 'Feedback', V[j - 1]) for j in range(1, 7)] + [
    ('PAN_MASTER', 'Panning', 'Master')] + [('PAN_V%d' % j, 'Panning', V[j - 1]) for j in range(1, 7)] + [
    ('LFO_RATE', 'Mod', 'LFO Rate'), ('LFO_SHAPE', 'Mod', 'LFO Shape'), ('LFO_PWIDTH', 'Mod', 'LFO PWidth'),
    ('LFO_DEPTH', 'Mod', 'LFO Depth'), ('ENVL_REL', 'Mod', 'Env L Release'), ('ENVR_REL', 'Mod', 'Env R Release'),
    ('AR_ATTACK', 'Mod', 'AR Attack'), ('AR_RELEASE', 'Mod', 'AR Release'), ('AR_MODE', 'Mod', 'AR Mode'),
    ('SW1_MODE', 'Mod', 'Sw 1 Mode'), ('SW1_RATE', 'Mod', 'Sw 1 Rate'), ('SW1_PWIDTH', 'Mod', 'Sw 1 PWidth'),
    ('SW2_MODE', 'Mod', 'Sw 2 Mode'), ('SW2_RATE', 'Mod', 'Sw 2 Rate'), ('SW2_PWIDTH', 'Mod', 'Sw 2 PWidth')]
PINDEX = {(pg, nm): i for i, (_, pg, nm) in enumerate(PARAMS)}
BYNAME = {}
for i, (_, pg, nm) in enumerate(PARAMS):
    BYNAME.setdefault(nm, []).append(i)

SOURCES = ['NONE', 'NOVALUE', 'ON', 'OFF', 'ADJUST', 'LFO', 'SINE', 'COSINE', 'SQUARE', 'SAWTOOTH', 'PULSE', 'TRIANGLE',
           'ENV_L', 'ENV_R', 'AR', 'LATCH', 'SW1', 'SW2', 'SW12', 'MONO_LVL', 'LEFT_LVL', 'RIGHT_LVL']
SRC_NAME = {'none': 'NONE', 'On': 'ON', 'Off': 'OFF', 'ADJUST': 'ADJUST', 'LFO': 'LFO', 'Sine': 'SINE',
            'Cosine': 'COSINE', 'Square': 'SQUARE', 'Sawtooth': 'SAWTOOTH', 'Pulse': 'PULSE', 'Triangle': 'TRIANGLE',
            'Env L': 'ENV_L', 'Env R': 'ENV_R', 'AR Env': 'AR', 'Latch': 'LATCH', 'Sw 1': 'SW1', 'Sw 2': 'SW2',
            'Sw 1&2': 'SW12', 'Mono Lvl': 'MONO_LVL', 'Left Lvl': 'LEFT_LVL', 'Right Lvl': 'RIGHT_LVL',
            'Tempo': 'OFF'}


def source(name):
    # MIDI, the footpedal and foot switches: no value in the module; the
    # Tempo source reads 0 (as the program data has it).
    return 'R80_SRC_' + SRC_NAME.get(name, 'NOVALUE')


def param_of(dest, algo):
    page, name = dest.split(':', 1)
    if (page, name) in PINDEX:
        return PINDEX[(page, name)]
    ids = BYNAME[name]
    assert len(ids) == 1, dest
    return ids[0]


def block_after(marker, n=1):
    """The n-th code block after the line holding marker."""
    at = spec.index(marker)
    for _ in range(n):
        at = spec.index('```', at) + 3
    end = spec.index('```', at)
    return spec[at:end]


# Our own laws in place of the tables the spec gives only as the unit's data
# (RT, Hz, gain, filter, glide and level scales, the Depth, Shape / Attack
# and slope tables, the Inverse's listed taps, the sine, the AP Sweep cycle,
# the Chorus divider): exact formulas where the spec has one, else fitted to
# the documented behaviour.
def poly(c, x):
    return sum(k * x ** i for i, k in enumerate(c))


def rnd(x):
    return int(math.floor(x + 0.5))


# Mid Rt v: RT(v) = h(v / 63) / (64 - v) ms
RT = [min(65535, rnd(poly((16308.73, 127555.1, -296237.9, 522332.3, -460247.7, 155877.8), v / 63) / (64 - v))) for v in range(64)]


def damp_b(v):
    """Rt HC / Crossover: b of y += (1 - b/256)(x - y), 4.4."""
    return 255 - v if v <= 8 else 247 - 2 * (v - 8) if v <= 14 else 235 - 5 * (v - 14)


FREQ = [rnd(-48000 * math.log(damp_b(v) / 256) / (2 * math.pi)) for v in range(60)]
assert all(rnd(256 * math.exp(-2 * math.pi * f / 48000)) == damp_b(v) for v, f in enumerate(FREQ))
TADJ = [v if v <= 10 else int(32767 * 10 ** ((v - 80) / 20)) for v in range(81)]
# Depth v: k13, k15, k16 in sixteenths
DEPTH = [[max(0, min(15, rnd(poly(c, v)))) for v in range(16)] for c in (
    (14.882, 0.14643, -0.078782), (0.02451, 0.18592, 0.054272), (4.9832, 0.041366, 0.15062, -0.011908))]
# High Cut v: -3 dB at f(v) = exp(sextic in v / 120) Hz; v = 121 open
HICUT = [rnd(2 ** 23 * (1 - math.exp(-2 * math.pi * math.exp(poly(
    (2.985865, 4.720479, 15.05452, -55.94223, 105.7109, -93.51013, 30.89163), v / 120)) / 48000)))
    for v in range(121)] + [2 ** 23 - 1]
# Shape / Attack curves c0..c3 (quartic in x / 256), c4 a ramp to 0 at x = 32;
# x = 0..256, values x 2^-15
CURVES = [[max(0, rnd(poly(c, x / 256))) for x in range(257)] for c in (
    (23944.8, 18353.3, -374846, 665066, -333117), (-1641.92, 64268.5, 87846.3, -346053, 201833),
    (1162.02, -17561.4, 225117, -357500, 163092), (-524.579, 27471.8, -95879.2, 188702, -98780.4))]
CURVES.append([max(0, 9000 * (32 - x) // 32) for x in range(257)])
# Inverse k17 / k51 by Mid: 128 up to 16, then cubic
J = [128] * 17 + [rnd(poly((127.41, -1.2405, -1.0212, 0.044892), m - 16)) for m in range(17, 33)]
GLIDE = list(range(1, 17)) + [rnd(math.exp(poly(
    (-2.085991, 66.44377, -342.8664, 961.9312, -1392.675, 998.2203, -280.8377), v / 100))) for v in range(16, 101)]
W = [rnd(63.5 + 63.35 * math.sin(2 * math.pi * (k + 0.5) / 512)) for k in range(128)]
LEVELMAP = [rnd(50 * p / 15) for p in range(16)] + [
    min(127, rnd(poly((-19.338, 13.5322, 1.03664), math.log2(p)))) for p in range(16, 128)]


def lfsr_shuffle(vals, seed):
    s, out = seed, list(vals)
    for i in range(len(out) - 1, 0, -1):
        u = 0
        for _ in range(16):
            bit = s & 1
            s >>= 1
            if bit:
                s ^= 0xB400
            u = (u << 1) | bit
        k = u % (i + 1)
        out[i], out[k] = out[k], out[i]
    return out


# AP Sweep period multipliers (x / 128): 11 steps from 64 to 192 spaced so
# their harmonic mean is the documented cycle's, in a shuffled order
SWEEP = lfsr_shuffle([rnd(64 + 128 * (k / 10) ** 0.7) for k in range(11)], 0x51C3)
# Concert Hall Chorus 1..10: the tap step divider D
DIVIDER = [0] + [max(1, rnd(math.exp(poly((4.07858, -0.0880069, -0.0151384), v)) - 4.1)) for v in range(1, 11)]


def offsets(marker):
    return {int(a): int(v) for a, v in re.findall(r'A(\d+): (\d+)', block_after(marker))}


PLATE_A0 = offsets('Plate Size-0 values A0')
CHAMBER_A0 = offsets('Chamber Size-0 values A0')
HALL = {int(a): (int(x), int(y), star == '*') for a, star, x, y in
        re.findall(r'A(\d+)(\*?): (\d+) → (\d+)', block_after('Concert Hall Size end points'))}
INV = {int(a): (int(x), int(y)) for a, x, y in re.findall(r'A(\d+): (\d+) → (\d+)', block_after('Duration end points'))}
# The Inverse's seven Spread / Shape taps: from their end points (Duration 0
# and 112), a step every second Duration
lt = block_after('Listed taps (value for Duration')
INV_LISTED = {}
for name, body in re.findall(r'A(\d+):\n((?:\s+[\d ]+\n?)+)', lt):
    ends = body.split()
    a0, a1 = int(ends[0]), int(ends[-1])
    INV_LISTED[int(name)] = [a0 + rnd((a1 - a0) * (d // 2) / 56) for d in range(113)]

progs = [p for p in data['programs'] if p['algorithm'] in ALGOS]


def stored_values(p):
    vals = [0] * len(PARAMS)
    locked = [0] * len(PARAMS)
    for pg in p['stored']['pages']:
        page = pg['page']
        for q in pg['params']:
            if page == '(no page)':
                i = BYNAME[q['name']][0]
            else:
                i = PINDEX[(page, q['name'])]
            vals[i] = q['value']
            locked[i] = int(q['locked'])
    for q in p['stored']['mod']:
        if q['name'] == 'Mix':
            continue
        i = PINDEX[('Mod', q['name'])]
        vals[i] = q['value']
        locked[i] = int(q['locked'])
    return vals, locked


def cname(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')


def short(name):
    s = name.upper()
    assert len(s) <= 12, s
    return s


# Hall modulated taps: the window position of each tap's second read (6.2).
TAP_B = (112, 118, 124, 4)

out_h = os.path.join(here, '..', 'src', 'r80_gen.h')
out_c = os.path.join(here, '..', 'src', 'r80_programs.c')
with open(out_h, 'w') as o:
    o.write('/* Generated by tools/r80gen.py from the 80\'s description; do not edit. */\n'
            '#ifndef R80_GEN_H_INCLUDED\n#define R80_GEN_H_INCLUDED\n\n'
            '/* Every stored parameter of the reverb algorithms (page order of the spec, 4.14). */\nenum r80_param {\n')
    for i, (e, pg, nm) in enumerate(PARAMS):
        o.write(f'    R80_P_{e},{" " * (12 - len(e))}/* {pg}: {nm} */\n')
    o.write('    R80_PARAMS\n};\n\n/* Patch and trigger sources (6.3); NOVALUE: MIDI, pedal, foot switches, tempo. */\n'
            'enum r80_source {\n')
    o.write(''.join(f'    R80_SRC_{s},\n' for s in SOURCES))
    o.write('    R80_SOURCES\n};\n\n#define R80_PATCHES_MAX 10\n#define R80_POINTS_MAX  8\n\n#endif\n')

recs = []
for p in progs:
    vals, locked = stored_values(p)
    st = p['stored']
    pt = st['patches']
    assert len(pt) <= 10
    patches = []
    for q in pt:
        pts = q['points']
        assert 2 <= len(pts) <= 8
        xs = ', '.join(str(x) for x, _ in pts)
        ys = ', '.join(str(y) for _, y in pts)
        patches.append(f'{{ {source(q["source"])}, R80_P_{PARAMS[param_of(q["destination"], p["algorithm"])][0]}, '
                       f'{len(pts)}, {int(q["tempo_coded"])}, {{ {xs} }}, {{ {ys} }} }}')
    trig = st['trigger']
    lat = st['latch']
    tap = [0, 0, 0, 0]
    if p['tank']['program'] == '5.3 Concert Hall':
        for j, s in enumerate(TAP_B):
            tap[j] = p['tank']['A'][str(s)] & 255
    adj = p['adjust']
    lockbits = [0, 0, 0, 0]
    for i, l in enumerate(locked):
        if l:
            lockbits[i // 32] |= 1 << (i % 32)
    recs.append(
        f'    {{ {p["number"]}, "{short(p["name"])}", {ALGOS[p["algorithm"]]}, {p["tempo_bpm"]},\n'
        f'      "{cname(adj["label"].upper())}", {adj["position"]}, {adj["max"]},\n'
        f'      {{ {", ".join(source(trig[k]["source"]) for k in ("AR", "Sw 1", "Sw 2"))} }}, '
        f'{{ {", ".join(str(trig[k]["level"]) for k in ("AR", "Sw 1", "Sw 2"))} }},\n'
        f'      {source(lat["source"])}, {lat["low"]}, {lat["high"]}, {{ {", ".join(map(str, tap))} }},\n'
        f'      {{ {", ".join(map(str, vals))} }},\n'
        f'      {{ {", ".join(hex(b) + "u" for b in lockbits)} }}, {len(patches)},\n'
        f'      {{ {(", ".join(patches)) or "{ 0 }"} }} }},\n')


def table(ctype, name, values, per=16):
    s = f'const {ctype} {name}[{len(values)}] = {{\n'
    for i in range(0, len(values), per):
        s += '    ' + ', '.join(str(v) for v in values[i:i + per]) + ',\n'
    return s + '};\n'


def sparse(name, d, n=127):
    v = [d.get(i, 0) for i in range(n)]
    return table('uint16_t', name, v)


with open(out_c, 'w') as o:
    o.write('/* Generated by tools/r80gen.py from the 80\'s description; do not edit. */\n'
            '#include "r80.h"\n\n/* Law tables (spec section 4, 6.3). */\n')
    o.write(table('uint16_t', 'r80_rt_ms', RT))
    o.write(table('uint16_t', 'r80_damp_hz', FREQ))
    o.write(table('uint16_t', 'r80_adjust_t', TADJ))
    o.write(table('uint8_t', 'r80_depth_16', DEPTH[0] + DEPTH[1] + DEPTH[2]))
    o.write(table('uint32_t', 'r80_hicut', HICUT, 8))
    o.write(table('uint16_t', 'r80_curves', sum(CURVES, []), 16))
    o.write(sparse('r80_plate_a0', PLATE_A0))
    o.write(sparse('r80_chamber_a0', CHAMBER_A0))
    o.write(sparse('r80_hall_a0', {a: v[0] for a, v in HALL.items()}))
    o.write(sparse('r80_hall_a1', {a: v[1] for a, v in HALL.items()}))
    o.write(table('uint8_t', 'r80_hall_half', [int(HALL.get(i, (0, 0, False))[2]) for i in range(127)]))
    o.write(table('uint8_t', 'r80_inv_j', J))
    o.write(sparse('r80_inv_a0', {a: v[0] for a, v in INV.items()}))
    o.write(sparse('r80_inv_a1', {a: v[1] for a, v in INV.items()}))
    keys = sorted(INV_LISTED)
    assert keys == [7, 8, 13, 14, 42, 46, 47]
    o.write(table('uint16_t', 'r80_inv_listed', sum((INV_LISTED[k] for k in keys), [])))
    o.write(table('uint16_t', 'r80_glide', GLIDE))
    o.write(table('uint8_t', 'r80_sine', W))
    o.write(table('uint8_t', 'r80_levelmap', LEVELMAP))
    o.write(table('uint8_t', 'r80_sweep', SWEEP))
    o.write(table('uint8_t', 'r80_divider', DIVIDER))
    o.write('\nconst struct r80_program r80_programs[] = {\n')
    o.write(''.join(recs))
    o.write('};\nconst unsigned r80_program_count = sizeof(r80_programs) / sizeof(r80_programs[0]);\n')
print(out_h)
print(out_c, len(recs), 'programs')

if expect:
    def flt(v):
        return repr(float(v))
    IO3 = ['aLL', 'aLR', 'aRL', 'aRR', 'gs', 'gr', 'postL', 'postR', 'pd', 'pr', 'c1', 'c2', 'g', 'vd', 'vp',
           'DV1', 'DV2', 'DV3', 'DV4'] + [f'V{j}{s}' for j in range(1, 5) for s in ('L', 'R', 'fb')] + \
          ['fv', 'fr', 'a', 'w1', 'w2', 'out_gain']
    IO31 = ['c90', 'c91', 'c92', 'c93', 'c94', 'c96', 'c97', 'c98', 'c99', 'c9A', 'c9B', 'c9C'] + \
           [f'B{j}' for j in range(1, 7)] + ['g'] + [f'A{j}' for j in range(1, 7)] + [f'R{j}' for j in range(1, 7)] + \
           ['c38', 'U', 'u', 'N'] + [f'cL{j}' for j in range(1, 7)] + [f'cR{j}' for j in range(1, 7)] + \
           ['cAC', 'cAD', 'cAE', 'cAF', 'out_gain']
    with open(expect, 'w') as o:
        o.write('/* Generated by tools/r80gen.py --expect: the program data\'s values. */\n'
                f'static const char *const expect_io3[] = {{ {", ".join(chr(34) + k + chr(34) for k in IO3)} }};\n'
                f'static const char *const expect_io31[] = {{ {", ".join(chr(34) + k + chr(34) for k in IO31)} }};\n'
                'struct expect { int number, tank; double k[127]; int has_k[127], a[127], has_a[127];\n'
                '    double io[64]; double param[%d]; int has_param[%d]; };\n' % (len(PARAMS), len(PARAMS)) +
                'static const struct expect expects[] = {\n')
        for p in progs:
            t = p['tank']
            k = [t['k'].get(str(i)) for i in range(127)]
            a = [t['A'].get(str(i)) for i in range(127)]
            io = p['io']['values']
            keys = IO31 if p['io']['section'] == '3.1' else IO3
            par = [None] * len(PARAMS)
            for q in p['parameters']:
                if 'effective' in q:
                    i = param_of((q['page'] or '(no page)') + ':' + q['name'], p['algorithm']) if q['page'] \
                        else BYNAME[q['name']][0]
                    par[i] = q['effective']
            o.write(f'    {{ {p["number"]}, {TANK[t["program"]]},\n'
                    f'      {{ {", ".join(flt(v or 0) for v in k)} }},\n'
                    f'      {{ {", ".join("1" if v is not None else "0" for v in k)} }},\n'
                    f'      {{ {", ".join(str(v or 0) for v in a)} }},\n'
                    f'      {{ {", ".join("1" if v is not None else "0" for v in a)} }},\n'
                    f'      {{ {", ".join(flt(io[x]) for x in keys)} }},\n'
                    f'      {{ {", ".join(flt(v or 0) for v in par)} }},\n'
                    f'      {{ {", ".join("1" if v is not None else "0" for v in par)} }} }},\n')
        o.write('};\n')
    print(expect)
