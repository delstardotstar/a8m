"""Writes fw/src/r70_gen.h, fw/src/r70_nets.h and fw/src/r70_programs.c:
the 70's eight networks as straight-line integer C, its algorithms'
parameters (control-value tables and the laws each network value follows),
its 84 factory programs as stored values and patches, the SIZE tables, the
walk sequences and the resampler kernels, from the description in
ref/reverb-70 (or DIR). Tables the description gives only as the unit's
data are not read: our own laws replace them (see "our own tables").

usage: python3 -I fw/tools/r70gen.py [--expect OUT.c] [DIR]
  --expect also writes every program's network and control values at rest
  and every parameter value's network values, for tools/r70test.c.
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
src = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'ref', 'reverb-70')
data = json.load(open(os.path.join(src, 'reverb-programs.json'), encoding='utf-8'))
spec = open(os.path.join(src, 'algorithms-and-programs.md'), encoding='utf-8').read()
NETS = ['chorus', 'chords', 'multiband', 'multiband_b', 'chamber', 'inverse', 'plate', 'hall']
nets = data['networks']

# The page's algorithms (UI order) and the JSON algorithms behind each.
ALGOS = [('Chorus + Echo', 'CHORUS+ECHO'), ('Chorus + Rhythm', 'CHORUS+RHYTHM'),
         ('Multiband Delay', 'MULTIBAND DLY'), ('Multiband Rhythm', 'MULTIBAND RHY'),
         ('Resonant Chords', 'RESON CHORDS'), ('Rhythmic Chords', 'RHYTHM CHORDS'),
         ('Concert Hall', 'CONCERT HALL'), ('Rich Chamber', 'RICH CHAMBER'),
         ('Infinite Chamber', 'INF CHAMBER'), ('Rich Plate', 'RICH PLATE'), ('Inverse Room', 'INVERSE ROOM')]
JALGOS = [(a['name'], a['set']) for a in data['algorithms']]
jalg = {(a['name'], a['set']): a for a in data['algorithms']}

# Edit slots (append only): enum suffix, yml key, the parameter names it edits.
EDITS = [
    ('SOFTKNOB', 'SoftKnob', ['SOFT KNOB']),
    ('SIZE', 'Size', ['SIZE M', 'DURATION']),
    ('RTMID', 'RtMid', ['RT MID S', 'REV TIME S']),
    ('RTLOW', 'RtLow', ['RT LOW S']),
    ('PDELAY', 'Predelay', ['PDELAY MS']),
    ('HC', 'HighCut', ['HC', 'HC L']),
    ('RTHC', 'RtHighCut', ['RT HC']),
    ('XOVER', 'Xover', ['XOVER']),
    ('DIFFUSION', 'Diffusion', ['DIFFUSION']),
    ('DEFINITION', 'Definition', ['DEFINITION']),
    ('ATTACK', 'Attack', ['ATTACK']),
    ('CHORUSING', 'Chorusing', ['CHORUSING']),
    ('CHORUS', 'Chorus', ['CHORUS']),
    ('RATE', 'Rate', ['RATE BPM']),
    ('DLYMST', 'DelayMaster', ['DLY MST']),
    ('FDBKMST', 'FeedbackMaster', ['FDBK MST']),
    ('LVLMST', 'LevelMaster', ['LVL MST']),
    ('PCHMST', 'PitchMaster', ['PCH MST']),
    ('RESNMST', 'ResonanceMaster', ['RESN MST']),
    ('PDLMST', 'PredelayMaster', ['PDL MST']),
    ('FB1', 'Feedback1', ['V1 FDBK %', 'V3 FDBK - %']),
    ('FB2', 'Feedback2', ['V2 FDBK %', 'V6 FDBK + %']),
    ('HCR', 'HighCutR', ['HC R']),
    ('LCMST', 'LowCutMaster', ['LC MST']),
    ('HCMST', 'HighCutMaster', ['HC MST']),
    ('LOSLOPE', 'LowSlope', ['LO SLOPE %']),
    ('MDSLOPE', 'MidSlope', ['MD SLOPE %']),
]
# Each algorithm's page fields, in order (SOFT KNOB first when patched).
FIELDS = {
    'Chorus + Echo': ['CHORUSING', 'CHORUS', 'DLYMST', 'FDBKMST', 'LVLMST', 'HC', 'DIFFUSION'],
    'Chorus + Rhythm': ['RATE', 'CHORUSING', 'CHORUS', 'FDBKMST', 'LVLMST', 'HC', 'DIFFUSION'],
    'Multiband Delay': ['DLYMST', 'FB1', 'FB2', 'LVLMST', 'LCMST', 'HCMST', 'DIFFUSION'],
    'Multiband Rhythm': ['RATE', 'FB1', 'FB2', 'LVLMST', 'LCMST', 'HCMST', 'DIFFUSION'],
    'Resonant Chords': ['PCHMST', 'RESNMST', 'PDLMST', 'LVLMST', 'HC', 'HCR', 'FB1', 'FB2'],
    'Rhythmic Chords': ['RATE', 'PCHMST', 'RESNMST', 'LVLMST', 'HC', 'HCR', 'FB1', 'FB2'],
    'Concert Hall': ['RTMID', 'RTLOW', 'SIZE', 'PDELAY', 'HC', 'RTHC', 'XOVER', 'DIFFUSION', 'DEFINITION',
                     'CHORUSING'],
    'Rich Chamber': ['RTMID', 'RTLOW', 'SIZE', 'PDELAY', 'HC', 'RTHC', 'XOVER', 'DIFFUSION', 'DEFINITION'],
    'Infinite Chamber': ['RTMID', 'SIZE', 'PDELAY', 'HC', 'DIFFUSION', 'DEFINITION', 'ATTACK'],
    'Rich Plate': ['RTMID', 'RTLOW', 'SIZE', 'PDELAY', 'HC', 'RTHC', 'XOVER', 'DIFFUSION', 'DEFINITION'],
    'Inverse Room': ['SIZE', 'MDSLOPE', 'LOSLOPE', 'PDELAY', 'HC', 'RTHC', 'DIFFUSION', 'DEFINITION', 'ATTACK'],
}
EDIT_IDX = {e[0]: i for i, e in enumerate(EDITS)}

ROLES = ['', 'size', 'rt_low', 'rt_mid', 'rtl_stop', 'rtm_stop', 'gate', 'decay_opt', 'definition', 'diffusion',
         'chorusing', 'chorus', 'rate']
CTLS = ['size', 'rt_low', 'rt_mid', 'rtl_stop', 'rtm_stop', 'gate', 'gate_hold', 'decay_opt', 'definition',
        'diffusion', 'chorusing', 'chorus']


def frac(c):
    """A fixed coefficient "31/32", "-1/2", "1": (units, resolution)."""
    s = 1
    if c.startswith('-'):
        s, c = -1, c[1:]
    if '/' in c:
        n, d = c.split('/')
        n, d = int(n), int(d)
        if d not in (8, 32):
            n, d = n * 32 // d, 32
        return s * n, d
    return s * int(c) * 32, 32


def is_sym(c):
    return not (c.lstrip('-')[0].isdigit())


# ---------------------------------------------------------------- our own tables
# No data the description lists only as a table of the unit's memory is used
# (the walk's direction sequences, the SIZE tables, the RT rate and ATTACK
# tables, value scales without a law): the laws below are ours, fitted to
# the documented behaviour, or follow from the listings and display values.
FS = 13000000.0 / 384.0


def walk_sequence(seed):
    """2 048 direction bytes from a 16-bit Galois LFSR (x^16 + x^14 + x^13 +
    x^11 + 1). Bit j is 1 with probability (UP[j] + o)/32, o drawn in -5..5
    per bit and run of 64 bytes, each decision from its own LFSR byte. The
    walk reads bits 1-6 as up / down: these rates keep the drift (a mean
    delay below the middle) and the slow swings of the unit's walk."""
    st = [seed]

    def draw():
        s, u = st[0], 0
        for _ in range(8):
            bit = s & 1
            s >>= 1
            if bit:
                s ^= 0xB400
            u = (u << 1) | bit
        st[0] = s
        return u

    out = []
    for i in range(2048):
        if i % 64 == 0:
            off = [draw() % 11 - 5 for _ in range(8)]
        byte = 0
        for j in range(8):
            byte |= (draw() < 8 * (UP[j] + off[j])) << j
        out.append(byte)
    return out


UP = (16, 16, 12, 11, 12, 12, 15, 11)
SEQ_SEEDS = (0x2F1D, 0x7A93)


def tempo(i):
    return int(60 * FS / (64 + i) + 0.5)


def rt_rate(i):
    u = i / 32.0
    return int(1.0 / (0.157185 - 0.562559 * u + 0.988863 * u ** 2 - 0.882922 * u ** 3 + 0.299649 * u ** 4) + 0.5)


# ATTACK rows (7.4): A(u), u = 3b/768, cubic.
ATTACK = {
    False: ((30.83, -6.6, -0.28, -25.12), (15.73, -3.42, 85.51, -98.85), (5.04, 14.83, -3.55, 14.28),
            (-1.02, 81.42, -73.36, 23.74)),
    True: ((30.32, 7.93, -45.1, 6.33), (10.65, -1.44, 76.35, -88.26), (1.79, 27.15, -1.56, -28.73),
           (1.42, 3.59, 25.99, 0.49)),
}


def attack_val(hall, row, b):
    u = 3 * b / 768.0
    c = ATTACK[hall][row]
    return max(0, min(31, int(math.floor(c[0] + c[1] * u + c[2] * u * u + c[3] * u ** 3 + 0.5))))


def size_listing(net):
    """The listing's SIZE-scaled delays at its three SIZE values (section 5)."""
    head = {'chamber': '### 5.5 ', 'inverse': '### 5.6 ', 'plate': '### 5.7 ', 'hall': '### 5.8 '}[net]
    at = spec.index('SIZE-scaled delays', spec.index(head))
    at = spec.index('```', at) + 3
    block = spec[at:spec.index('```', at)].strip().splitlines()
    ss = [int(x) for x in re.findall(r's=(\d+)', block[0])[:3]]
    pts = {}
    for line in block[1:-1]:
        for name, a, b, c in re.findall(r'(t\d+)\s+(\d+)\s+(\d+)\s+(\d+)', line):
            pts[name] = (int(a), int(b), int(c))
    counts = [int(x) for x in re.search(r'count ([\d ]+)/([\d ]+)/([\d ]+)', block[-1]).groups()]
    return ss, pts, counts


HALL_KNEE_S = 17     # the Hall's input diffusers stop growing here


def size_rows(net):
    """Per SIZE s = 0..max: (f1, {delay: samples}, max_count), interpolated in f1."""
    sz = nets[net]['size']
    beta, cap = sz['b'], sz['cap']
    ss, pts, counts = size_listing(net)
    f1 = [16 * min(beta + s, cap) // beta for s in range(ss[2] + 1)]
    knee = f1[HALL_KNEE_S] if net == 'hall' else None

    def interp(f, xs, ys):
        if f <= xs[1]:
            x0, x1, y0, y1 = xs[0], xs[1], ys[0], ys[1]
        else:
            x0, x1, y0, y1 = xs[1], xs[2], ys[1], ys[2]
        return y0 if x1 == x0 else y0 + (y1 - y0) * (f - x0) / (x1 - x0)

    xs = [f1[s] for s in ss]
    rows = []
    for s in range(ss[2] + 1):
        dl = {}
        for k in sorted(pts, key=lambda t: int(t[1:])):
            y = pts[k]
            if knee and y[1] == y[2] and y[0] * f1[-1] > 32 * y[2]:
                v = y[0] + (y[2] - y[0]) * (min(f1[s], knee) - xs[0]) / (knee - xs[0])
            else:
                v = interp(f1[s], xs, y)
            dl[k] = max(1, int(math.floor(v + 0.5)))
        rows.append((f1[s], dl, int(math.floor(interp(f1[s], xs, counts)))))
    return rows


SIZE = {k: size_rows(k) for k in NETS if 'size' in nets[k]}
TAP_OFFSET = 1                       # d = min(count, C) + 1 (7.3)
MOD_OFFSET = {'hall': (0, -127)}     # c0 of the Hall's modulators (9.1)

NOTES = ['C', 'D!', 'D', 'E!', 'E', 'F', 'G!', 'G', 'A!', 'A', 'B!', 'B']
PERCENT = {'DIFFUSION', 'ATTACK', 'DEFINITION', 'CHORUSING', 'V1 FDBK %', 'V2 FDBK %', 'V3 FDBK - %', 'V6 FDBK + %'}


def last_number(text):
    return float(re.findall(r'\d*\.?\d+', text)[-1])


def level_b(text):
    """6.3: b = 4n, n = min(63, round(64 * 10^(dB/20))); -3 dB has n = 46."""
    if 'OFF' in text:
        return 0
    if 'FULL' in text:
        return 252
    db = -last_number(text)
    n = 46 if db == -3.0 else min(63, int(math.floor(64 * 10 ** (db / 20) + 0.5)))
    return 4 * n


def base_tempo(a):
    prog = next(q for q in data['programs'] if q['set'] == a['set'] and q['pos'] == a['base_program'])
    rate = next(q for q in a['params'] if q.get('role') == 'rate')
    return tempo(prog['values'][rate['pos']] - rate['lo'])


_scales = {}


def own_scale(a, p):
    """The parameter's control values b and counts per stored value, and the
    control-value tables that differ from b, by the laws of 6.3 / 6.5; None
    where the law is the affine one the data file lists."""
    key = (a['name'], a['set'], p['pos'])
    if key in _scales:
        return _scales[key]
    name = re.sub(r' \d$', '', p['name'])
    vs = p['values']
    texts = [x['text'].strip() for x in vs]
    idx = [x['v'] - p['lo'] for x in vs]
    hall = a['network'] == 'hall'
    b = count = None
    ctl = {}
    if name in PERCENT:
        assert p['hi'] - p['lo'] == 99
        b = [254 * i // 99 for i in idx]
    elif name in ('V FDBK %', 'V RESN %'):
        b = [128 + (127 * x['v']) // 97 for x in vs]
    elif name in ('V LVL', 'RFL'):
        b = [level_b(t) for t in texts]
    elif name == 'V PAN':
        b = [min(254, 255 * i // 100) for i in idx]
    elif name == 'V LC':
        b = [32 * ((x['v'] + 16) // 4) if a['set'] == 'A' else 8 * (x['v'] + 15) for x in vs]
    elif name == 'CHORUS':
        b = [0 if i == 0 else i if i <= 6 else 128 + i - 6 for i in idx]
    elif name == 'REV TIME S':
        b = [254 if t == 'INF' else 8 * i for i, t in zip(idx, texts)]
    elif name == 'GATE':
        b = [(192 if hall else 128) + (t != 'OFF') for t in texts]
        ctl = {'gate_hold': [254 if t == 'OFF' else min(254, i) for i, t in zip(idx, texts)],
               'gate': [int(t != 'OFF') for t in texts]}
    elif name == 'DECAY OPT':
        ctl = {'decay_opt': idx}
    elif name in ('V DLY MS', 'RFL MS'):
        count = [int(math.floor(last_number(t) * FS / 1000 + 0.5)) for t in texts]
    elif name == 'V PITCH':
        count = []
        for t in texts:
            midi = 12 * (int(t[-1]) + 1) + NOTES.index(t[:-1].strip())
            count.append(int(math.floor(FS / (440 * 2 ** ((midi - 69) / 12)) + 0.5)) - 1)
    elif p.get('rhythm'):
        r = p['rhythm']
        tb = base_tempo(a) // 24
        n = [x['v'] - p['lo'] for x in vs]
        if r['unit'] == 128:
            b = [min(196, 2 * ((k * tb + 128) // 256)) for k in n]
        else:
            mb = r.get('max_beats_count')
            count = [(min(k, mb // tb) if mb else k) * tb for k in n]
    out = {'b': b, 'count': count, 'ctl': ctl}
    for k in ('b', 'count'):
        if out[k] is None and any(k in x for x in vs):
            assert affine([(i, x[k]) for i, x in zip(idx, vs)]), (a['name'], p['name'], k)
            out[k] = [x[k] for x in vs]
    _scales[key] = out
    return out


# ---------------------------------------------------------------- laws (7.x)
def law_candidates(net):
    hall = net == 'hall'
    L = [('X8', 0, lambda b, c: b >> 3), ('NX8', 0, lambda b, c: -(b >> 3)),
         ('Y32', 0, lambda b, c: min(31, 32 - (b >> 3))), ('YLC', 0, lambda b, c: 32 - (b >> 3)),
         ('Y8', 0, lambda b, c: min(7, (32 - (b >> 3)) >> 2)), ('X32', 0, lambda b, c: b >> 5),
         ('NX32', 0, lambda b, c: -(b >> 5)),
         ('FB', 0, lambda b, c: ((b - 128) >> 4) if b >= 128 else -min(7, (131 - b) >> 4)),
         ('RES', 0, lambda b, c: max(-31, min(31, (b - 128) >> 2))),
         ('DEF', 0, lambda b, c: min(8, (5 * (63 - (b >> 2))) >> 3)),
         ('HCRTY', 0, lambda b, c: 32 if b < 8 else 64 - (b >> 3)),
         ('G2', 0, lambda b, c: (5 * b) >> 6), ('G4', 0, lambda b, c: b >> 2),
         ('OTAP', 0, lambda b, c: 5 if b > 128 else 3), ('INP', 0, lambda b, c: 0 if b >= 253 else 20),
         ('GINF', 0, lambda b, c: 32 if b >= 253 else b >> 3),
         ('PDL', 0, lambda b, c: 128 * b + 1)]
    for s in (6, 5, 4, 3):
        L.append(('DIF', s, lambda b, c, s=s: min(26, (s * (b >> 2)) >> 3)))
        L.append(('NDIF', s, lambda b, c, s=s: -min(26, (s * (b >> 2)) >> 3)))
    for r in range(4):
        L.append(('ATT32', r, lambda b, c, r=r: attack_val(hall, r, b)))
        L.append(('NATT32', r, lambda b, c, r=r: -attack_val(hall, r, b)))
        L.append(('ATT8', r, lambda b, c, r=r: min(attack_val(hall, r, b), 31) >> 2))
    return L


def dlyh(count, h):
    return (257 * h if (count >> 8) > h else count) + 1


LAWS = ['X8', 'NX8', 'Y32', 'YLC', 'Y8', 'X32', 'NX32', 'FB', 'RES', 'DEF', 'HCRTY', 'G2', 'G4', 'OTAP', 'INP',
        'GINF', 'PDL', 'DIF', 'NDIF', 'ATT32', 'NATT32', 'ATT8', 'DLYH']


# ---------------------------------------------------------------- symbols per network
class Net:
    def __init__(self, name):
        n = nets[name]
        self.name = name
        self.n = n
        self.coefs = list(n['coefficients'])
        self.res = {k: v['resolution'] for k, v in n['coefficients'].items()}
        self.delays = list(n['delays'])
        # ramped products: their own coefficient slots
        # (signal, term, c, res, delay, bypass); in 1/32 always (exact at rest, finer ramps)
        self.slots = []
        self.slot_of = {}
        for d, prods in n.get('delay_ramp', {}).get('products', {}).items():
            for p in prods:
                self.slot_of[(p['signal'], p['term'])] = len(self.slots)
                self.slots.append([p['signal'], p['term'], p['c'], 32, d, 0])
        # the products that read a tap the DIFFUSION 0 bypass replaces ramp too
        froms = [r['from'] for r in n.get('bypass', {}).get('replace', [])]
        for sg in n['signals']:
            st = sg['start']
            prods = [('start', st['c'] if isinstance(st, dict) and 'of' in st else '1',
                      st['of'] if isinstance(st, dict) and 'of' in st else st)]
            prods += [(i, t['c'], t['of']) for i, t in enumerate(sg['terms'])]
            for term, c, op in prods:
                if op in froms:
                    key = (sg['name'], term)
                    if key not in self.slot_of:
                        self.slot_of[key] = len(self.slots)
                        self.slots.append([sg['name'], term, c, 32, None, 0])
                    self.slots[self.slot_of[key]][5] = 1
        self.nc = len(self.coefs) + len(self.slots)
        rt = n.get('rt')
        self.rt_syms = (['shelf'] + list(rt['inloop'])) if rt else []

    def ci(self, s):
        return self.coefs.index(s)

    def di(self, s):
        return self.delays.index(s)


NETOBJ = {k: Net(k) for k in NETS}


def delay_bounds(net):
    """The largest value each delay symbol of the network can take."""
    n = nets[net]
    hi = {}
    if net in SIZE:
        for f1, dl, mc in SIZE[net]:
            for k, v in dl.items():
                hi[k] = max(hi.get(k, 0), v)
            for k in n['delays']:
                if k == 'pd' or k.startswith('rd'):
                    hi[k] = max(hi.get(k, 0), mc + TAP_OFFSET)
    for m in n.get('modulators', []):
        hi[m['A']] = hi[m['B']] = 256
    for a in data['algorithms']:
        if a['network'] != net:
            continue
        for p in a['params']:
            cnt = own_scale(a, p)['count']
            for x in p['values']:
                for k, v in x.get('set', {}).items():
                    if k in n['delays']:
                        hi[k] = max(hi.get(k, 0), v, (max(cnt) + 1) if cnt else 0)
            dl = p.get('delay_law')
            for t in p.get('taps', []):
                if dl and 'max_high' in dl:
                    hi[t] = max(hi.get(t, 0), 257 * dl['max_high'] + dl['offset'], max(cnt or [0]) + 1)
                r = p.get('rhythm')
                if r:
                    hi[t] = max(hi.get(t, 0), (128 * 196 if r['unit'] == 128 else 257 * r['max_high']) + r['offset'])
        for k, v in a['base'].items():
            if k in n['delays']:
                hi[k] = max(hi.get(k, 0), v)
    for d in n['delays']:
        assert d in hi, (net, d)
    return hi


# ---------------------------------------------------------------- network code
def sign_classes(net):
    """Per coefficient symbol: '+' never negative, '-' never positive, '±'
    either; and whether it can reach 1 (a start that saturates)."""
    N = NETOBJ[net]
    vals = {k: set() for k in N.coefs}
    for a in data['algorithms']:
        if a['network'] != net:
            continue
        for k, v in a['base'].items():
            if k in vals:
                vals[k].add(v)
        for p in a['params']:
            for x in p['values']:
                for k, v in x.get('set', {}).items():
                    if k in vals:
                        vals[k].add(v)
    for m in N.n.get('modulators', []):
        vals[m['k']] |= {0, 32}
    for k in N.rt_syms:
        vals[k] |= {0, 16}
    if N.rt_syms:
        vals['shelf'] |= {-31, 31}
    out = {}
    for k, vs in vals.items():
        vs = vs or {0}
        sg = '+' if min(vs) >= 0 else '-' if max(vs) <= 0 else '±'
        out[k] = (sg, max(abs(v) for v in vs) >= N.res[k])
    return out


def gen_net(net):
    N = NETOBJ[net]
    n = N.n
    bound = delay_bounds(net)
    sigs = n['signals']
    stored = [s['name'] for s in sigs if s.get('stored')]
    byp = {}
    if 'bypass' in n:
        for r in n['bypass']['replace']:
            byp[json.dumps(r['from'], sort_keys=True)] = r['to']
    # ring sizes
    need = {s: 0 for s in stored}

    def dmax(d, add=0):
        return (d if isinstance(d, int) else bound[d]) + add

    def visit(op):
        if isinstance(op, dict) and 'tap' in op:
            need[op['tap']] = max(need[op['tap']], dmax(op['d'], op.get('add', 0)))
            to = byp.get(json.dumps(op, sort_keys=True))
            if to:
                visit(to)

    for s in sigs:
        ops = [s['start']] + [t['of'] for t in s['terms']]
        if isinstance(s['start'], dict) and 'of' in s['start']:
            ops[0] = s['start']['of']
        for o in ops:
            visit(o)
    # Mirrored rings of 32-bit words (a write lands twice, a read never
    # wraps), holding the tap value 4w + 3 itself.
    rings, off = {}, 0
    for s in stored:
        size = 8
        while size < need[s] + 1:
            size *= 2
        rings[s] = (off, size, len(rings))
        off += 2 * size
    used_syms = set()
    cls = sign_classes(net)
    # Rings this long are likely out of cache by the time a tap reads them:
    # each tap's run of the next call is prefetched.
    PF_SIZE = 256
    pf = []

    def prefetch(j, size, de):
        near = de.isdigit() and any(k == j and e.isdigit() and abs(int(e) - int(de)) < 8 for k, e in pf)
        if size >= PF_SIZE and (j, de) not in pf and not near:
            pf.append((j, de))

    def tap(op):
        o, size, j = rings[op['tap']]
        d = op['d']
        if isinstance(d, str):
            used_syms.add(('d', N.di(d)))
            de = 'D%d' % N.di(d)
            if op.get('add'):
                de = '(D%d + (%d))' % (N.di(d), op['add'])
            r = 'RB%d[-(int32_t)%s]' % (j, de)
            prefetch(j, size, '(int32_t)' + de)
        else:
            r = 'RB%d[-%d]' % (j, d + op.get('add', 0))
            prefetch(j, size, str(d + op.get('add', 0)))
        to = byp.get(json.dumps(op, sort_keys=True))
        if to:
            r = '(byp ? %s : %s)' % (tap(to), r)
        return r

    def operand(op):
        if op == 'x':
            return 'x'
        if isinstance(op, str):
            return op
        return tap(op)

    def coef(c, slot=None):
        """('k', idx, class, sat) for a symbol or slot, ('f', units, res) for a fixed value."""
        if slot is not None:
            k = len(N.coefs) + slot
            sym = N.slots[slot][2]
            used_syms.add(('c', k))
            if is_sym(sym):
                c0 = cls[sym]
                return ('k', k, c0[0], c0[1], False)
            u, r = frac(sym)
            return ('k', k, '+' if u >= 0 else '-', abs(u) >= r, False)
        neg = c.startswith('-')
        name = c[1:] if neg else c
        if is_sym(c):
            k = N.ci(name)
            used_syms.add(('c', k))
            sg, sat = cls[name]
            if neg:
                sg = {'+': '-', '-': '+', '±': '∓'}[sg]
            return ('k', k, sg, sat, neg)
        u, r = frac(c)
        return ('f', u, r)

    def fixed(cf, y):
        u, r = cf[1], cf[2]
        if abs(u) == r:
            return y
        if abs(u) * 2 == r:
            return '((%s) >> 1)' % y
        return '((%d * (%s)) >> %d)' % (abs(u), y, 3 if r == 8 else 5)

    lines = []
    for s in sigs:
        nm = s['name']
        st = s['start']
        slot = N.slot_of.get((nm, 'start'))
        if isinstance(st, dict) and 'of' in st or slot is not None:
            y = operand(st['of'] if isinstance(st, dict) and 'of' in st else st)
            cf = coef(st['c'] if isinstance(st, dict) and 'of' in st else '1', slot)
            if cf[0] == 'f':
                p = fixed(cf, y)
                expr = ('(~%s)' % p) if cf[1] < 0 else p
                if abs(cf[1]) > cf[2]:
                    expr = 'SAT(%s)' % expr
            else:
                k, sg, sat, negated = cf[1], cf[2], cf[3], cf[4]
                p = '((M%d * (%s)) >> 8)' % (k, y)
                if sg == '+':
                    expr = p
                else:
                    used_syms.add(('n' if negated else 's', k))
                    expr = '(%s ^ %s%d)' % (p, 'N' if negated else 'S', k)
                if sat:
                    expr = 'SAT(%s)' % expr
            if s.get('start_at_unity'):
                k = cf[1]
                expr = '(M%d >= 256 ? SAT(%s + (((M%d - 256) * (%s)) >> 8)) : %s)' % (
                    k, s['start_at_unity'], k, y, expr)
        else:
            expr = operand(st)
        lines.append('        int32_t %s = %s;' % (nm, expr))
        for ti, t in enumerate(s['terms']):
            cf = coef(t['c'], N.slot_of.get((nm, ti)))
            y = operand(t['of'])
            if cf[0] == 'f':
                p = fixed(cf, y)
                lines.append('        %s = SAT(%s %s %s);' % (nm, nm, '-' if cf[1] < 0 else '+', p))
                continue
            k, sg = cf[1], cf[2]
            p = '((M%d * (%s)) >> 8)' % (k, y)
            if sg == '+':
                lines.append('        %s = SAT(%s + %s);' % (nm, nm, p))
            elif sg == '-':
                lines.append('        %s = SAT(%s - %s);' % (nm, nm, p))
            else:
                v = ('S%d' if sg == '±' else 'N%d') % k
                used_syms.add(('s' if sg == '±' else 'n', k))
                lines.append('        %s = SAT(%s + TERM(%s, %s));' % (nm, nm, p, v))
        if s.get('interp'):
            ip = s['interp']
            k = N.ci(ip['k'])
            lines.append('        if (M%d == 256) %s = %s;' % (k, nm, tap(ip['B'])))
        if s.get('stored'):
            o, size, j = rings[nm]
            lines.append('        RB%d[0] = RB%d[-%d] = %s | 3;' % (j, j, size, nm))
            prefetch(j, size, '0')
            prefetch(j, size, str(size))
    outs = n['outputs']
    head = ['static void net_%s(struct r70_run *r, const int32_t *in, int32_t (*out)[2], unsigned count)' % net,
            '{', '    int32_t *const pool = r70_pool;', '    uint32_t t = r->t;', '    const int byp = r->bypass;']
    for kind, i in sorted(used_syms):
        if kind == 'c':
            head.append('    const int32_t M%d = r->mag[%d];' % (i, i))
        elif kind == 's':
            head.append('    const int32_t S%d = r->sgn[%d];' % (i, i))
        elif kind == 'n':
            head.append('    const int32_t N%d = r->neg[%d];' % (i, i))
        else:
            head.append('    const uint32_t D%d = r->delay[%d];' % (i, i))
    head.append('    (void)byp;')
    if pf:
        head.append('    {')
        head.append('        const uint32_t n = t + 2u * count - 1u;')
        for j in sorted({j for j, _ in pf}):
            o, size, _ = next(v for v in rings.values() if v[2] == j)
            head.append('        const int32_t *const P%d = pool + %d + (n & %du);' % (j, o + size, size - 1))
        for j, de in sorted(pf, key=lambda x: x[0]):
            head.append('        __builtin_prefetch(P%d - %s);' % (j, de))
        head.append('    }')
    head.append('    for (unsigned f = 0; f < count; f++, t++) {')
    head.append('        const int32_t x = in[f];')
    for s_ in stored:
        o, size, j = rings[s_]
        head.append('        int32_t *const RB%d = pool + %d + (t & %du);' % (j, o + size, size - 1))
    tail = ['        out[f][0] = %s;' % outs[0], '        out[f][1] = %s;' % outs[1], '    }', '    r->t = t;', '}', '']
    return head + lines + tail, off, rings


# ---------------------------------------------------------------- parameters
def affine(xs):
    """(a, c) with y = a*i + c for every (i, y), or None."""
    if len(xs) < 2:
        return (0, xs[0][1]) if xs else (0, 0)
    (i0, y0), (i1, y1) = xs[0], xs[1]
    if (y1 - y0) % (i1 - i0):
        return None
    a = (y1 - y0) // (i1 - i0)
    c = y0 - a * i0
    return (a, c) if all(y == a * i + c for i, y in xs) else None


tables = []          # int32 values; pooled
table_at = {}


def table(vals):
    key = tuple(vals)
    if key not in table_at:
        table_at[key] = len(tables)
        tables.extend(vals)
    return table_at[key]


targets = []         # (sym kind/index, law, arg)
texts = []           # string pool (indexes)
text_at = {}


def text_table(vals):
    key = tuple(vals)
    if key not in text_at:
        text_at[key] = len(texts)
        texts.extend(vals)
    return text_at[key]


def param_record(a, p, net):
    N = NETOBJ[net]
    vs = p['values']
    lo = p['lo']
    rec = {'pos': int(p['pos'][0]) * 10 + int(p['pos'][2]), 'lo': lo, 'hi': p['hi'],
           'role': ROLES.index(p.get('role', '')) if p.get('role', '') in ROLES else 0,
           'master': p['pos'][2] == '0' and p['name'].endswith('MST')}
    own = own_scale(a, p)
    for key in ('b', 'count'):
        ys = own[key]
        rec['has_' + key] = ys is not None
        if ys is not None:
            af = affine(list(enumerate(ys)))
            rec[key] = ('a',) + af if af else ('t', table(ys))
    ctl = []
    for cname in CTLS:
        if not any(cname in x.get('control', {}) for x in vs):
            continue
        ctl.append((CTLS.index(cname), table(own['ctl'][cname]) if cname in own['ctl'] else -1))
    rec['ctl'] = ctl
    # laws per network symbol: the candidate closest to the listed values
    tg = []
    syms = []
    for x in vs:
        for k in x.get('set', {}):
            if k not in syms:
                syms.append(k)
    bs = own['b'] or [None] * len(vs)
    cs = own['count'] or [None] * len(vs)
    for k in syms:
        if k in N.rt_syms:
            continue
        pts = [(b, c, x['set'][k]) for b, c, x in zip(bs, cs, vs) if k in x.get('set', {})]
        found = None
        if k in N.delays:
            dl = p.get('delay_law', {})
            if 'max_high' in dl and all(c is not None for _, c, _ in pts):
                found = ('DLYH', dl['max_high'])
            elif all(b is not None and 128 * b + 1 == y for b, _, y in pts):
                found = ('PDL', 0)
        else:
            best = None
            for name, arg, fn in law_candidates(net):
                if any(b is None for b, _, _ in pts):
                    break
                err = sum(abs(fn(b, None) - y) for b, _, y in pts)
                if best is None or err < best:
                    found, best = (name, arg), err
            if best is None or best > len(pts):
                found = None
        assert found, (a['name'], p['name'], k, pts[:5])
        kind = 1 if k in N.delays else 0
        idx = N.di(k) if kind else N.ci(k)
        tg.append((kind, idx, LAWS.index(found[0]), found[1]))
    rec['tgt'] = len(targets)
    rec['ntgt'] = len(tg)
    targets.extend(tg)
    rec['taps'] = [N.di(t) for t in p.get('taps', []) if 'count' in vs[0] and p.get('role') != 'size'
                   and 'size' in nets[net] and t in N.delays]
    r = p.get('rhythm')
    rec['rhythm'] = (r['unit'], r['offset'], r.get('max_high', 0), r.get('max_beats_count', 0),
                     [N.di(t) for t in p['taps']]) if r else None
    rec['name'] = p['name']
    return rec


# ---------------------------------------------------------------- build
algos = []
for name, s in JALGOS:
    a = jalg[(name, s)]
    net = a['network']
    N = NETOBJ[net]
    recs = [param_record(a, p, net) for p in a['params']]
    # base values not set by any law
    set_by = set()
    for rec in recs:
        for kind, idx, law, arg in targets[rec['tgt']:rec['tgt'] + rec['ntgt']]:
            set_by.add((kind, idx))
        for d in rec['taps']:
            set_by.add((1, d))
        if rec['rhythm']:
            for d in rec['rhythm'][4]:
                set_by.add((1, d))
    if net in SIZE:
        for d in SIZE[net][0][1]:
            set_by.add((1, N.di(d)))
        for d in N.delays:
            if d == 'pd' or d.startswith('rd'):
                set_by.add((1, N.di(d)))
    for m in N.n.get('modulators', []):
        set_by |= {(0, N.ci(m['k'])), (1, N.di(m['A'])), (1, N.di(m['B']))}
    for k in N.rt_syms:
        set_by.add((0, N.ci(k)))
    base = []
    for k, v in a['base'].items():
        kind = 1 if k in N.delays else 0
        idx = N.di(k) if kind else N.ci(k)
        if (kind, idx) not in set_by:
            base.append((kind, idx, v))
    rtmid = next((p for p in a['params'] if p.get('role') == 'rt_mid'), None)
    rtsyms = [N.ci(k) for k in (rtmid or {}).get('rt_symbols', [])]
    ui = next(i for i, (nm, _) in enumerate(ALGOS) if nm == name)
    fields = []
    for e in FIELDS[name]:
        pnames = EDITS[EDIT_IDX[e]][2]
        p = next(p for p in a['params'] if p['name'] in pnames)
        fields.append((EDIT_IDX[e], a['params'].index(p)))
    sk = next(p for p in a['params'] if p['name'] == 'SOFT KNOB')
    fields.insert(0, (EDIT_IDX['SOFTKNOB'], a['params'].index(sk)))
    for e, pi in fields:
        p = a['params'][pi]
        recs[pi]['text'] = text_table([x['text'].strip() for x in p['values']])
    algos.append({'name': name, 'set': s, 'net': NETS.index(net), 'ui': ui, 'recs': recs, 'base': base,
                  'rtsyms': rtsyms, 'fixed': a['fixed_control'], 'fields': fields, 'params': a['params']})

progs = sorted(data['programs'], key=lambda p: (p['set'], p['pos']))
names = [p['name'] for p in progs]
dup = {n for n in names if names.count(n) > 1}


def prog_name(p):
    n = ' '.join(p['name'].split())
    if n in dup and p['set'] == 'B':
        n = (n[:10] + ' B')
    assert len(n) <= 12
    return n


# ---------------------------------------------------------------- resampler kernels
FS70 = 13000000.0 / 384.0
PHASES = 128


def kaiser(n, beta):
    def i0(x):
        s, t, k = 1.0, 1.0, 1
        while t > 1e-12 * s:
            t *= (x / (2 * k)) ** 2
            s += t
            k += 1
        return s
    return [i0(beta * math.sqrt(max(0.0, 1 - (2 * i / (n - 1) - 1) ** 2))) / i0(beta) for i in range(n)]


def kernel(taps, unit_hz, fc, beta):
    """(PHASES + 1) x taps, phase j: the taps at offsets k - taps/2 + 1 - j/PHASES (input samples of unit_hz),
    each phase summing to 1."""
    out = []
    for j in range(PHASES + 1):
        frac_ = j / PHASES
        row = []
        for k in range(taps):
            t = (k - taps // 2 + 1) - frac_            # in input samples
            x = t / unit_hz                            # seconds
            u = 2 * fc * x
            s = 1.0 if abs(u) < 1e-12 else math.sin(math.pi * u) / (math.pi * u)
            pos = (t + taps // 2 - 1 + frac_) / (taps - 1) if taps > 1 else 0
            w = 0.0
            if 0 <= pos <= 1:
                w = kaiser_w(pos, beta)
            row.append(s * w)
        sm = sum(row)
        out.append([v / sm for v in row])
    return out


def kaiser_w(pos, beta):
    def i0(x):
        s, t, k = 1.0, 1.0, 1
        while t > 1e-12 * s:
            t *= (x / (2 * k)) ** 2
            s += t
            k += 1
        return s
    return i0(beta * math.sqrt(max(0.0, 1 - (2 * pos - 1) ** 2))) / i0(beta)


DOWN_TAPS, UP_TAPS, FC, BETA = 64, 48, 16900.0, 8.0
kdown = kernel(DOWN_TAPS, 48000.0, FC, BETA)
kup = kernel(UP_TAPS, FS70, FC, BETA)

# ---------------------------------------------------------------- write
gen_h = ['/* Generated by tools/r70gen.py from the 70\'s description; do not edit. */',
         '#ifndef R70_GEN_H_INCLUDED', '#define R70_GEN_H_INCLUDED', '']
gen_h.append('enum r70_net { %s, R70_NETS };' % ', '.join('R70_NET_' + k.upper() for k in NETS))
gen_h.append('enum r70_page_algo { %s, R70_ALGOS };' % ', '.join(
    'R70_' + n.upper().replace(' + ', '_').replace(' ', '_') for n, _ in ALGOS))
gen_h.append('enum r70_edit { %s, R70_EDITS };' % ', '.join('R70_E_' + e[0] for e in EDITS))
gen_h.append('enum r70_role { %s };' % ', '.join('R70_R_' + (r.upper() or 'NONE') for r in ROLES))
gen_h.append('enum r70_ctl { %s, R70_CTLS };' % ', '.join('R70_C_' + c.upper() for c in CTLS))
gen_h.append('enum r70_law { %s };' % ', '.join('R70_L_' + l for l in LAWS))
maxc = max(N.nc for N in NETOBJ.values())
maxd = max(len(N.delays) for N in NETOBJ.values())
maxp = max(len(a['recs']) for a in algos)
maxslots = max(len(N.slots) for N in NETOBJ.values())
net_code, pool = [], 0
ringinfo = {}
for k in NETS:
    code, size, rings = gen_net(k)
    net_code += code
    pool = max(pool, size)
    ringinfo[k] = size
gen_h += ['#define R70_COEFS       %d' % maxc, '#define R70_DELAYS      %d' % maxd,
          '#define R70_PARAMS      %d' % maxp, '#define R70_SLOTS       %d' % maxslots,
          '#define R70_POOL        %d' % pool, '#define R70_FIELDS_MAX  %d' % (max(len(f) for f in FIELDS.values()) + 1),
          '#define R70_PATCHES_MAX 10', '#define R70_PHASES      %d' % PHASES,
          '#define R70_DOWN_TAPS   %d' % DOWN_TAPS, '#define R70_UP_TAPS     %d' % UP_TAPS, '', '#endif', '']

c = ['/* Generated by tools/r70gen.py from the 70\'s description; do not edit. */', '#include "r70.h"', '']


def arr(ctype, name, vals, per=16):
    out = ['%s %s[%d] = {' % (ctype, name, len(vals))]
    for i in range(0, len(vals), per):
        out.append('    ' + ', '.join(str(v) for v in vals[i:i + per]) + ',')
    out.append('};')
    return out


c += arr('const int32_t', 'r70_tables', tables or [0])
c += ['const struct r70_target r70_targets[%d] = {' % max(1, len(targets))]
c += ['    { %d, %d, %d, %d },' % t for t in targets] or ['    { 0 }']
c += ['};']
c += ['const char *const r70_texts[%d] = {' % len(texts)]
for i in range(0, len(texts), 8):
    c.append('    ' + ' '.join('"%s",' % t for t in texts[i:i + 8]))
c += ['};', '']

for ai, a in enumerate(algos):
    c.append('static const struct r70_param params_%d[%d] = {' % (ai, len(a['recs'])))
    for rec in a['recs']:
        def ab(key):
            v = rec.get(key)
            if not v:
                return '0, 0, -1'
            return '%d, %d, -1' % (v[1], v[2]) if v[0] == 'a' else '0, 0, %d' % v[1]
        ctl = rec['ctl'] + [(0, -1)] * (2 - len(rec['ctl']))
        assert len(rec['ctl']) <= 2
        rh = rec['rhythm']
        taps = rec['taps'] + [0] * (6 - len(rec['taps']))
        rtaps = (rh[4] if rh else []) + [255] * (6 - len(rh[4] if rh else []))
        flags = (1 if rec['has_b'] else 0) | (2 if rec['has_count'] else 0) | (4 if rec['master'] else 0)
        c.append('    { %d, %d, %d, %d, %d, %s, %s, %d, { { %d, %d }, { %d, %d } }, %d, %d, %d, { %s }, '
                 '%d, %d, %d, %d, { %s }, %d, "%s" },' % (
                     rec['pos'], rec['lo'], rec['hi'], rec['role'], flags, ab('b'), ab('count'), len(rec['ctl']),
                     ctl[0][0], ctl[0][1], ctl[1][0], ctl[1][1], rec['tgt'], rec['ntgt'], len(rec['taps']),
                     ', '.join(map(str, taps)), rh[0] if rh else 0, rh[1] if rh else 0, rh[2] if rh else 0,
                     rh[3] if rh else 0, ', '.join(map(str, rtaps)), rec.get('text', -1), rec['name']))
    c.append('};')
    c.append('static const struct r70_base base_%d[%d] = {' % (ai, max(1, len(a['base']))))
    c += ['    { %d, %d, %d },' % b for b in a['base']] or ['    { 0, 0, 0 }']
    c.append('};')
c.append('const struct r70_algo r70_algos[%d] = {' % len(algos))
for ai, a in enumerate(algos):
    fx_ = a['fixed']
    rts = a['rtsyms'] + [255] * (2 - len(a['rtsyms']))
    c.append('    { "%s", %d, %d, %d, params_%d, %d, base_%d, %d, { %d, %d }, %d, %d, %d, %d, %d, %d, %d, '
             '{ %s }, { %s } },' % (
                 a['name'], 1 if a['set'] == 'B' else 0, a['net'], a['ui'], ai, len(a['recs']), ai, len(a['base']),
                 rts[0], rts[1], 'rt_low' in fx_, fx_.get('rt_low', 0), 'chorus' in fx_, fx_.get('chorus', 0),
                 'decay_opt' in fx_, int(fx_.get('decay_opt', 0)), len(a['fields']),
                 ', '.join('%d' % e for e, _ in a['fields']), ', '.join('%d' % p for _, p in a['fields'])))
c.append('};')
c.append('const char *const r70_algo_names[R70_ALGOS] = { %s };' % ', '.join('"%s"' % u for _, u in ALGOS))
c.append('const char *const r70_edit_keys[R70_EDITS] = { %s };' % ', '.join('"%s"' % e[1] for e in EDITS))
c.append('')

# networks
for ni, k in enumerate(NETS):
    N = NETOBJ[k]
    n = N.n
    c.append('static const struct r70_slot slots_%s[%d] = {' % (k, max(1, len(N.slots))))
    for sig, term, cc, res, d, byp_ in N.slots:
        di = N.di(d) if d else 255
        if is_sym(cc):
            assert not cc.startswith('-')
            c.append('    { %d, %d, 0, %d, %d },' % (di, N.ci(cc), res, byp_))
        else:
            u, r = frac(cc)
            c.append('    { %d, 255, %d, %d, %d },' % (di, u * 32 // r, res, byp_))
    if not N.slots:
        c.append('    { 0 }')
    c.append('};')
    if k in SIZE:
        tb = SIZE[k]
        tds = list(tb[0][1])
        offs = [d for d in N.delays if d == 'pd' or d.startswith('rd')]
        c += arr('static const uint16_t', 'size_%s' % k, [r[1][d] for r in tb for d in tds])
        c += arr('static const uint16_t', 'sizemax_%s' % k, [r[2] for r in tb])
        c += arr('static const uint8_t', 'sizef1_%s' % k, [r[0] for r in tb])
        c += arr('static const uint8_t', 'sizetd_%s' % k, [N.di(d) for d in tds])
        c += arr('static const uint8_t', 'sizeoff_%s' % k, [N.di(d) for d in offs] + [TAP_OFFSET] * len(offs))
c.append('const struct r70_netinfo r70_netinfo[R70_NETS] = {')
for k in NETS:
    N = NETOBJ[k]
    n = N.n
    mods = n.get('modulators', [])
    mm = ', '.join('{ %d, %d, %d, %d, %d, %d, %d }' % (N.ci(m['k']), N.di(m['A']), N.di(m['B']), m['a0'], m['b0'],
                                                        m['S0'], m.get('offset', 0)) for m in mods) or '{ 0 }'
    det = n.get('detector', {})
    rt = n.get('rt', {})
    inl = list(rt.get('inloop', {}).items())
    inl += [('', 0)] * (2 - len(inl))
    grp = n.get('delay_ramp', {}).get('group')
    sz = SIZE.get(k)
    moff = MOD_OFFSET.get(k, (0, 0))
    resl = [N.res.get(cname, 32) for cname in N.coefs] + [s[3] for s in N.slots]
    c.append('    { %d, %d, %d, slots_%s, %d, { %s }, %d, %d, %d, %d, %d, %d, %d, %d, { %d, %d }, { %d, %d }, '
             '%d, %d, %d, %d, %s, %s, %s, %s, %s, %d, %d, %d, %d, %d, { %d, %d }, { %s } },' % (
                 len(N.coefs), len(N.delays), len(N.slots), k, len(mods), mm,
                 1 if det else 0, det.get('e', 0), det.get('c', 0), det.get('rt_update_samples', 0),
                 det.get('rt_update_arrive', 0), rt.get('stages', 0), N.ci('shelf') if rt else 255,
                 len(rt.get('inloop', {})), N.ci(inl[0][0]) if inl[0][0] else 255,
                 N.ci(inl[1][0]) if inl[1][0] else 255, inl[0][1], inl[1][1],
                 N.di(grp['delay']) if grp else 255, grp['first'] if grp else 0, grp['pass'] if grp else 0,
                 grp['rest'] if grp else 0,
                 'size_' + k if sz else '0', 'sizemax_' + k if sz else '0', 'sizef1_' + k if sz else '0',
                 'sizetd_' + k if sz else '0', 'sizeoff_' + k if sz else '0',
                 len(sz[0][1]) if sz else 0, sum(d == 'pd' or d.startswith('rd') for d in N.delays) if sz else 0,
                 (len(sz) - 1) if sz else 0, n['size']['b'] if sz else 0, n['size']['cap'] if sz else 0, moff[0], moff[1],
                 ', '.join('%d' % r for r in resl)))
c.append('};')
for k in NETS:
    N = NETOBJ[k]
    if 'size' in N.n:
        c.append('_Static_assert(R70_COEFS >= %d, "coefs");' % N.nc)
c.append('')

# programs
c.append('const struct r70_program r70_programs[%d] = {' % len(progs))
for i, p in enumerate(progs):
    ai = JALGOS.index((p['algorithm'], p['algorithm_set']))
    a = algos[ai]
    vals = [p['values'][q['pos']] for q in a['params']]
    pts = p['patches']
    assert len(pts) <= 10
    c.append('    { %d, "%s", %d, %d, %d, %d, { %s }, %d, { %s } },' % (
        i + 1, prog_name(p), a['ui'], ai, 1 if p['set'] == 'B' else 0, len(vals), ', '.join(map(str, vals)),
        len(pts), ', '.join('{ %d, %d, %d }' % (q['source'], q['dest'], q['scale']) for q in pts) or '{ 0 }'))
c.append('};')
c.append('const unsigned r70_program_count = %d;' % len(progs))
c += arr('const uint16_t', 'r70_tempo', [tempo(i) for i in range(128)])
c += arr('const uint16_t', 'r70_rt_rate', [rt_rate(i) for i in range(32)])
c.append('const uint8_t r70_attack[2][4][256] = {')
for hall in (False, True):
    c.append('    {')
    for r in range(4):
        c.append('        { ' + ', '.join(str(attack_val(hall, r, b)) for b in range(256)) + ' },')
    c.append('    },')
c.append('};')
c.append('const uint8_t r70_sequence[2][2048] = {')
for seed in SEQ_SEEDS:
    sq = walk_sequence(seed)
    c.append('    {')
    for i in range(0, 2048, 32):
        c.append('        ' + ', '.join(str(v) for v in sq[i:i + 32]) + ',')
    c.append('    },')
c.append('};')
c.append('const float r70_fxadj[103] = { %s };' % ', '.join(
    '%.7ff' % 10 ** (-0.39 * (((102 - L) * 10) // 4 - 30) / 20) for L in range(103)))
c.append('const float r70_kdown[R70_PHASES + 1][R70_DOWN_TAPS] __attribute__((aligned(16))) = {')
for row in kdown:
    c.append('    { ' + ', '.join('%.9ef' % v for v in row) + ' },')
c.append('};')
c.append('const float r70_kup[R70_PHASES + 1][R70_UP_TAPS] __attribute__((aligned(16))) = {')
for row in kup:
    c.append('    { ' + ', '.join('%.9ef' % v for v in row) + ' },')
c.append('};')

nets_h = ['/* Generated by tools/r70gen.py from the listings of the 70\'s description (section 5); do not edit. */',
          '/* Ring words used per network: %s */' % ', '.join('%s %d' % (k, ringinfo[k]) for k in NETS)] + net_code
nets_h.append('static void (*const r70_net_fn[R70_NETS])(struct r70_run *, const int32_t *, int32_t (*)[2], unsigned) = {')
nets_h.append('    ' + ', '.join('net_' + k for k in NETS))
nets_h.append('};')

fw = os.path.join(here, '..', 'src')
open(os.path.join(fw, 'r70_gen.h'), 'w').write('\n'.join(gen_h))
open(os.path.join(fw, 'r70_programs.c'), 'w').write('\n'.join(c) + '\n')
open(os.path.join(fw, 'r70_nets.h'), 'w').write('\n'.join(nets_h) + '\n')
print('pool %d words, %d tables, %d targets, %d texts' % (pool, len(tables), len(targets), len(texts)))

if expect:
    e = ['/* Generated by tools/r70gen.py --expect; the program data. */']
    e.append('struct expect { int ncoef, ndelay; int coef[%d]; int delay[%d]; int ctl[%d]; };' % (maxc, maxd, len(CTLS)))
    e.append('static const struct expect expects[%d] = {' % len(progs))
    for p in progs:
        ai = JALGOS.index((p['algorithm'], p['algorithm_set']))
        N = NETOBJ[jalg[(p['algorithm'], p['algorithm_set'])]['network']]
        nv = p['network_values']
        cf = [nv.get(k, -99999) for k in N.coefs] + [0] * (maxc - len(N.coefs))
        dl = [nv.get(k, -99999) for k in N.delays] + [0] * (maxd - len(N.delays))
        ct = [int(p['control'].get(k, 0)) for k in CTLS]
        e.append('    { %d, %d, { %s }, { %s }, { %s } },' % (len(N.coefs), len(N.delays), ', '.join(map(str, cf)),
                                                          ', '.join(map(str, dl)), ', '.join(map(str, ct))))
    e.append('};')
    # every parameter value's network values (others at the base program)
    rows = []
    for ai, a in enumerate(algos):
        N = NETOBJ[NETS[a['net']]]
        for pi, p in enumerate(a['params']):
            for x in p['values']:
                for k, v in x.get('set', {}).items():
                    if k in N.rt_syms:
                        continue
                    kind = 1 if k in N.delays else 0
                    rows.append((ai, pi, x['v'], kind, N.di(k) if kind else N.ci(k), v))
    bases = []
    for a in algos:
        bpos = jalg[(a['name'], a['set'])]['base_program']
        bases.append(next(i for i, q in enumerate(progs) if q['set'] == a['set'] and q['pos'] == bpos))
    e.append('static const int pbase[%d] = { %s };' % (len(bases), ', '.join(map(str, bases))))
    e.append('struct pexpect { short algo, param, v, kind, idx; int value; };')
    e.append('static const struct pexpect pexpects[%d] = {' % len(rows))
    for r in rows:
        e.append('    { %d, %d, %d, %d, %d, %d },' % r)
    e.append('};')
    open(expect, 'w').write('\n'.join(e) + '\n')
