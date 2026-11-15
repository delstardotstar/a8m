/* The 70's engine: its network at 33 854.17 Hz in blocks of 8 samples
 * (section 10.4), with the level detector (8), the delay modulation (9),
 * delay ramps (7.3) and the bus resamplers. The laws run in the
 * foreground (r70_poll) and hand their values to the audio. */
#include "r70.h"

#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#define SUB             8u
#define IN_RING         128u
#define OUT_RING        128u
#define LATENCY         64          /* unit samples between the network's output and the bus */
#define CLEAR_WORDS     16384u
#define FADE_STEP       (1.0f / 512.0f)
#define INFADE_STEP     (1.0f / 1024.0f)
#define GAIN_SMOOTH     0.0386f     /* 1 - exp(-8 / (6 ms x fs)) */
#define HP_A            0.99996845f /* 1 - 2 pi 0.17 / fs */
#define SEND_SCALE      (1.0f / 2147483648.0f)
#define WET_SCALE       32768.0f    /* a 16-bit output sample to Q30 */

#if defined(__arm__)
#define SAT(v) __builtin_arm_ssat((v), 18)
#else
#define SAT(v) ((v) > 131071 ? 131071 : (v) < -131072 ? -131072 : (v))
#endif
#define TERM(p, s) (((p) ^ (s)) - (s))

int32_t r70_pool[R70_POOL] __attribute__((aligned(64)));

#include "r70_nets.h"

/* ---- foreground to audio ---- */

static struct {
    const struct r70_program *load;
    struct r70_values tgt[2];
    volatile uint32_t load_seq, tgt_seq;
    volatile uint8_t tgt_idx;
} hand;

enum { F_MID = 1, F_LOW = 2, F_STOP = 4 };
enum { RAMP_IDLE, RAMP_DOWN, RAMP_UP };
enum { OWN_NONE, OWN_DETECTOR, OWN_WALK };

/* Per block of 8, in 1/256: an edit's coefficient slew, the detector's
 * values' slew, a per-tap ramp's step, a group ramp pass's step. */
#define SLEW            2
#define SLEW_DETECTOR   8
#define RAMP_TAP        4
#define RAMP_GROUP      8

struct rewrite { uint32_t due; int16_t l, m; int8_t n; };

static struct {
    uint32_t load_seen, tgt_seen;
    const struct r70_program *p, *next;
    const struct r70_algo *a;
    const struct r70_netinfo *ni;
    struct r70_values v;            /* the latest values handed over */
    struct r70_run run;
    int16_t cur[R70_COEFS];         /* the detector's and the walk's coefficients, in their units */
    int16_t fine[R70_COEFS];        /* every coefficient in the network now, 1/256 */
    uint8_t owned[R70_COEFS];       /* OWN_* */
    uint8_t scale[R70_COEFS];       /* 256 / resolution */
    uint8_t moving;                 /* something to slew or ramp: ramps() runs */
    uint32_t clearing;              /* words cleared + 1, 0 done */
    uint8_t ready, restart, setb;
    float fade;
    /* resamplers */
    float in_ring[2 * IN_RING] __attribute__((aligned(16)));
    float out_ring[2][2 * OUT_RING] __attribute__((aligned(16)));
    uint32_t in_n, p_int, p_rem;        /* inputs taken; the next unit sample's position */
    int32_t q_int;
    uint32_t q_rem, out_n;              /* the next bus sample's position; unit outputs made */
    int32_t xq[2 * SUB];
    unsigned nx;
    float hp_x[2], hp_y[2], gain, infade;
    /* delay ramps */
    uint8_t phase[R70_DELAYS], rd[R70_DELAYS], nrs[R70_DELAYS], rs[R70_DELAYS][2];
    uint16_t smag[R70_SLOTS], bmag[R70_SLOTS];
    uint8_t bphase, bs[R70_SLOTS];
    unsigned nrd, nbs;
    int32_t gtimer;
    int16_t ge;
    /* detector (8) */
    uint32_t k, busy, ticks;
    int32_t peak;            /* the input's peak follower, 1/256 of a 16-bit step */
    int rt_goal, lambda, pk, prev, avg, held, tail, hist[11], flags, hold, curl, curm, cnt8, trig, n, P, P0, cnt, Q, frozen;
    struct rewrite q[48];
    unsigned nq;
    /* walk (9) */
    int64_t budget;
    uint8_t ma[6], mb[6], ms[6], ml[6], r, cnt64, cntdir, stepcnt, B, D, nact, modet;
    uint16_t seq;
} st;

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static void set_coef(unsigned k, int v)
{
    st.run.mag[k] = v < 0 ? -v : v;
    st.run.sgn[k] = v < 0 ? -1 : 0;
    st.run.neg[k] = v > 0 ? -1 : 0;
}

/* ---- detector ---- */

static int loglev(int x, int fast)
{
    int p = 0, q;

    if (!x) {
        return 23;
    }
    while (x >> (p + 1)) {
        p++;
    }
    q = p ? (x >> (p - 1)) & 1 : fast;
    return (16 * p + 8 * q + 135) & 255;
}

static void rates(void)
{
    int f1 = st.ni->size ? st.ni->sizef1[imin(st.v.ctl[R70_C_SIZE], st.ni->smax)] : 16;
    int alpha = imin(255, (st.ni->det_e * f1) >> 4);

    st.Q = (2 * r70_rt_rate[st.curm >> 3] * alpha + 128) >> 8;
    if (st.Q < 1) {
        st.Q = 1;
    }
    st.P0 = imin(255, imax(1, ((st.frozen ? 4 * f1 : f1) * st.ni->det_c) >> 8));
}

static void push(uint32_t due)
{
    if (st.nq < sizeof(st.q) / sizeof(st.q[0])) {
        st.q[st.nq++] = (struct rewrite){ due, (int16_t)st.curl, (int16_t)st.curm, (int8_t)st.n };
    }
}

/* A tick's RT recompute number *k (8.5): arrives at k Tr + Tw. */
static void recompute(uint32_t k0, int *k)
{
    rates();
    push(k0 + (uint32_t)(*k * st.ni->tr + st.ni->tw));
    (*k)++;
    st.n = 1;
}

static void apply_rewrites(void)
{
    unsigned i = 0;

    while (i < st.nq && st.q[i].due <= st.k) {
        r70_rt(st.a, st.q[i].l, st.q[i].m, st.v.ctl[R70_C_DEFINITION], st.q[i].n, st.cur);
        st.moving = 1;
        i++;
    }
    if (i) {
        memmove(st.q, st.q + i, (st.nq - i) * sizeof(st.q[0]));
        st.nq -= i;
    }
}

static int attack_now(int L) { return L >= st.avg && (L > 200 || L - st.avg >= 39); }

static void fast_tick(uint32_t k0, int *k)
{
    int lam = st.lambda;

    if (st.frozen || (!st.n && !st.flags)) {
        return;
    }
    st.pk = imax(st.pk, lam);
    if (lam > st.prev) {
        int L = loglev(lam, 1);
        if (st.n > 0 && L >= st.tail >> 8) {
            st.tail = 256 * (L - (st.tail >> 8));
            st.trig = 0;
            st.P = st.P0;
            st.n = 0;
            push(k0 + (uint32_t)(*k * st.ni->tr));
        }
        if (st.flags && attack_now(L)) {
            st.held = L;
            st.hold = st.v.ctl[R70_C_GATE_HOLD];
            if (st.curm < st.v.ctl[R70_C_RT_MID]) {
                st.curl = st.v.ctl[R70_C_RT_LOW];
                st.curm = st.v.ctl[R70_C_RT_MID];
                recompute(k0, k);
                st.flags = 0;
            }
        }
    }
    st.prev = lam;
}

static int move_rt(int *cur, int target)
{
    int d = 8 * (*cur >> 3);

    if (target >= d) {
        *cur = target;
        return 1;
    }
    *cur = d - 8;
    return target >= *cur;
}

static void slow_tick(uint32_t k0, int *k)
{
    int L = loglev(imax(st.pk, st.lambda), 0), Lt = st.frozen ? 16 : L;

    st.pk = 0;
    if (Lt >= st.tail >> 8) {
        st.tail = 256 * Lt;
        st.trig = 0;
        if (st.n > 0) {
            st.P = st.P0;
            st.n = 0;
            push(k0 + (uint32_t)(*k * st.ni->tr));
        }
    }
    st.avg = (st.avg + (L >> 3) - (st.avg >> 3)) & 255;
    if (attack_now(L)) {
        st.held = L;
        st.hold = st.v.ctl[R70_C_GATE_HOLD];
        if (st.flags & F_STOP) {
            st.flags = F_MID | F_LOW;
        }
    } else {
        st.held = imax(st.held, L);
        if (st.held - L >= 5 && st.v.ctl[R70_C_GATE] && !(st.flags & F_STOP)) {
            st.flags = F_MID | F_LOW | F_STOP;
        }
    }
    if (st.v.ctl[R70_C_DECAY_OPT]) {
        if ((st.tail >> 8) - Lt >= 22 && !st.trig) {
            st.trig = 1;
            if ((st.tail >> 8) - st.hist[0] >= 24) {
                st.P = 15;
            }
        }
    }
    memmove(st.hist, st.hist + 1, 10 * sizeof(st.hist[0]));
    st.hist[10] = st.tail >> 8;
    while (st.flags & (F_MID | F_LOW)) {
        int stop = st.flags & F_STOP;
        if (st.flags & F_MID) {
            if (stop && st.hold > 0) {
                st.hold--;
                break;
            }
            if (move_rt(&st.curm, stop ? st.v.ctl[R70_C_RTM_STOP] : st.v.ctl[R70_C_RT_MID])) {
                st.flags &= ~F_MID;
            }
        }
        if (st.flags & F_LOW) {
            if (stop && st.hold > 0 && !(st.flags & F_MID)) {
                st.hold--;
                break;
            }
            if (move_rt(&st.curl, stop ? st.v.ctl[R70_C_RTL_STOP] : st.v.ctl[R70_C_RT_LOW])) {
                st.flags &= ~F_LOW;
            }
        }
        recompute(k0, k);
    }
    if (!st.frozen && st.Q >= 120) {
        st.P = 20;
    }
    if (--st.cnt <= 0) {
        st.cnt = st.P;
        if (!st.trig && st.n > 0) {
            st.n--;
            push(k0 + (uint32_t)(*k * st.ni->tr));
        } else if (st.trig && st.n < (st.frozen ? 20 : 12)) {
            st.n++;
            push(k0 + (uint32_t)(*k * st.ni->tr));
        }
    }
    if (--st.cnt8 <= 0) {
        st.cnt8 = 8;
        if (!st.frozen) {
            st.tail = imax(0, st.tail - 65536 / st.Q);
            st.held = imax(0, st.held - 3);
        }
    }
}

static void detector_reset(void)
{
    st.pk = st.prev = st.avg = st.held = st.tail = st.flags = st.hold = st.trig = st.n = st.P = 0;
    memset(st.hist, 0, sizeof(st.hist));
    st.cnt = 1;
    st.cnt8 = 8;
    st.curl = st.v.ctl[R70_C_RT_LOW];
    st.curm = st.v.ctl[R70_C_RT_MID];
    st.frozen = st.curm >= 253;
    st.nq = 0;
    st.rt_goal = 0;
    st.busy = 0;
    st.peak = 0;
    st.lambda = 0;
    rates();
    r70_rt(st.a, st.curl, st.curm, st.v.ctl[R70_C_DEFINITION], 0, st.cur);
    st.n = 1;
    st.P = st.P0;
}

/* The detector over a block's samples (8.5): the peak follower per
 * sample, the ticks after the samples outside the busy time. Returns how
 * many samples were outside it. */
static unsigned detector_run(const int32_t *x)
{
    unsigned free = 0;

    for (unsigned f = 0; f < SUB; f++) {
        int a = ((x[f] - 3) >> 2), m = (a < 0 ? -a : a) << 8, d = st.peak - ((st.peak * 44) >> 16);
        st.peak = m > d ? m : d;
        st.lambda = imin(254, (st.peak * 254) >> 23);
        st.k++;
        if (st.k <= st.busy) {
            continue;
        }
        free++;
        if (!st.ni->det) {
            continue;
        }
        st.ticks++;
        int kk = 0;
        if (!(st.ticks & 63)) {
            fast_tick(st.k, &kk);
        }
        if (!(st.ticks % 609)) {
            slow_tick(st.k, &kk);
        }
        if (kk) {
            st.busy = st.k + (uint32_t)(kk * st.ni->tr);
        }
    }
    return free;
}

/* ---- delay modulation (9) ---- */

static void walk_config(void)
{
    int v = st.v.ctl[R70_C_CHORUSING] >> 3, c = st.v.ctl[R70_C_CHORUS];

    st.B = (uint8_t)(v == 0 ? 0 : v < 16 ? 17 - v : 1);
    st.D = (uint8_t)(v < 16 ? 1 : v - 15);
    st.nact = (uint8_t)imin(c & 15, st.ni->nmods);
    st.modet = st.ni->nmods == 6 && c >= 128;
}

static void walk_reset(void)
{
    for (unsigned j = 0; j < st.ni->nmods; j++) {
        st.ma[j] = st.ni->mods[j].a0;
        st.mb[j] = st.ni->mods[j].b0;
        st.ms[j] = st.ni->mods[j].S0;
        st.ml[j] = 1;
    }
    st.seq = 16;
    st.r = r70_sequence[st.setb][16];
    st.cnt64 = st.cntdir = 1;
    st.stepcnt = 0;
    st.budget = 0;
    walk_config();
}

static void walk_publish(unsigned j)
{
    const struct r70_mod *m = &st.ni->mods[j];
    int f = st.ms[j] & 63, c0 = st.ni->size ? st.ni->moff[j] : m->offset;

    st.cur[m->k] = (int16_t)(f == 32 ? 32 : f & 31);
    st.fine[m->k] = (int16_t)(st.cur[m->k] * 8);
    set_coef(m->k, st.fine[m->k]);
    st.run.delay[m->A] = (uint32_t)(c0 + st.ma[j]);
    st.run.delay[m->B] = (uint32_t)(c0 + st.mb[j]);
}

/* One modulator's step; returns 1 if it leapfrogged. */
static int walk_step(unsigned j, int up)
{
    int L = st.ml[j], leap = 0, half = st.ni->nmods == 2;

    for (int guard = 0; guard < 4; guard++) {
        int S = st.ms[j], b = st.mb[j], p, isa;
        if (up) {
            if (S >= 128) {
                if (S + L <= 160) { st.ms[j] = (uint8_t)(S + L); return leap; }
                p = b + 1; isa = 1;
            } else if (S == 0) {
                p = b + 2; isa = 0;
            } else {
                st.ms[j] = (uint8_t)imax(S - L, 0);
                return leap;
            }
        } else {
            if (S >= 128) {
                int s2 = (S - L) & 255;
                if (s2 >= 128) { st.ms[j] = (uint8_t)s2; return leap; }
                p = b - 2; isa = 0;
            } else {
                if (S + L <= 32) { st.ms[j] = (uint8_t)(S + L); return leap; }
                p = b - 1; isa = 1;
            }
        }
        if (p < 0 || p > 255 || (half && (p >> 7) != (b >> 7))) {
            st.r ^= (uint8_t)(1u << ((j + 1) & 7));
            return leap;
        }
        if (isa) {
            st.ma[j] = (uint8_t)p;
        } else {
            st.mb[j] = (uint8_t)p;
        }
        st.ms[j] ^= 128;
        leap = 1;
    }
    return leap;
}

/* One update (9.2); its duration in 1/192000 sample (9.3). */
static int32_t walk_update(void)
{
    if (!st.B) {
        return 1411 * 2000;
    }
    if (!--st.cnt64) {
        st.cnt64 = 64;
        for (unsigned j = 0; j < st.nact; j++) {
            int b = st.mb[j];
            st.ml[j] = (uint8_t)(st.modet ? st.D : 1 + (((64 + (imin(b, 255 - b) >> 1)) * 2 * st.D) >> 8));
        }
        if (!--st.cntdir) {
            st.cntdir = 4;
            st.seq = (uint16_t)((st.seq + 1) & 2047);
            st.r = r70_sequence[st.setb][st.seq];
        }
    }
    st.stepcnt--;
    if (st.stepcnt) {
        return 1509 * 2000;
    }
    st.stepcnt = st.B;
    int32_t d = (665 + 262 + (st.modet ? 0 : 5 * (st.D - 1))) * 2000;
    for (unsigned j = 0; j < st.nact; j++) {
        int up = (st.r >> ((j + 1) & 7)) & 1;
        uint8_t a = st.ma[j], b = st.mb[j], S = st.ms[j];
        d += 1185000 + (walk_step(j, up) ? 1008000 : 0);
        if (a != st.ma[j] || b != st.mb[j] || S != st.ms[j]) {
            walk_publish(j);
        }
    }
    return d;
}

static void walk_run(unsigned free)
{
    if (!st.ni->nmods || !st.nact) {
        return;
    }
    st.budget += (int64_t)free * 96 * (st.ni->nmods == 6 ? 1738 : 1712);
    while (st.budget > 0) {
        st.budget -= walk_update();
    }
}

/* ---- coefficient slews and delay ramps ---- */

static int scale(unsigned k) { return st.scale[k]; }

static int slot_rest(unsigned s)
{
    const struct r70_slot *sl = &st.ni->slots[s];

    return sl->sym == 255 ? sl->value * 8 : st.fine[sl->sym];
}

/* A ramp of products down, a switch, and back up (7.3); returns 1 at the
 * switch. mag[] holds the ramp's magnitudes. */
static int ramp_step(uint8_t *phase, int differs, const uint8_t *list, unsigned n, uint16_t *mag, int step)
{
    int all0 = 1, rest = 1;

    if (*phase == RAMP_UP && differs) {
        *phase = RAMP_DOWN;
    }
    for (unsigned j = 0; j < n; j++) {
        unsigned s = list[j];
        int r = slot_rest(s), mr = r < 0 ? -r : r;
        if (*phase == RAMP_DOWN) {
            mag[s] = (uint16_t)imax(mag[s] - step, 0);
            all0 &= !mag[s];
        } else {
            mag[s] = (uint16_t)imin(mag[s] + step, mr);
            rest &= mag[s] == mr;
        }
    }
    if (*phase == RAMP_DOWN && all0) {
        *phase = RAMP_UP;
        return 1;
    }
    if (*phase == RAMP_UP && rest) {
        *phase = RAMP_IDLE;
    }
    return 0;
}

static void ramp_start(uint8_t *phase, const uint8_t *list, unsigned n, uint16_t *mag)
{
    *phase = RAMP_DOWN;
    for (unsigned j = 0; j < n; j++) {
        int r = slot_rest(list[j]);
        mag[list[j]] = (uint16_t)(r < 0 ? -r : r);
    }
}

static int toward(int v, int t, int step)
{
    return v < t ? imin(v + step, t) : imax(v - step, t);
}

static void ramps(void)
{
    const struct r70_netinfo *ni = st.ni;
    unsigned nc = ni->ncoef;
    int byp = st.v.ctl[R70_C_DIFFUSION] == 0, moving = 0;

    if (!st.moving) {
        return;
    }
    for (unsigned k = 0; k < nc; k++) {
        int t = (st.owned[k] == OWN_NONE ? st.v.coef[k] : st.cur[k]) * scale(k);
        switch (st.owned[k]) {
        case OWN_NONE: st.fine[k] = (int16_t)toward(st.fine[k], t, SLEW); break;
        case OWN_DETECTOR: st.fine[k] = (int16_t)toward(st.fine[k], t, SLEW_DETECTOR); break;
        default: break;     /* the walk writes its own */
        }
        moving |= st.fine[k] != t;
    }
    for (unsigned i = 0; i < st.nrd; i++) {
        unsigned d = st.rd[i], group = d == ni->gdelay;
        int differs = st.run.delay[d] != st.v.delay[d];

        if (st.phase[d] == RAMP_IDLE) {
            if (!differs) {
                continue;
            }
            ramp_start(&st.phase[d], st.rs[i], st.nrs[i], st.smag);
            st.gtimer = ni->gfirst;
        }
        if (group) {
            st.gtimer -= (int32_t)SUB;
            if (st.gtimer > 0) {
                continue;
            }
            st.gtimer += ni->gpass;
        }
        if (ramp_step(&st.phase[d], differs, st.rs[i], st.nrs[i], st.smag, group ? RAMP_GROUP : RAMP_TAP)) {
            st.run.delay[d] = st.v.delay[d];
        }
        moving |= st.phase[d] != RAMP_IDLE;
    }
    if (st.nbs) {
        if (st.bphase == RAMP_IDLE && byp != st.run.bypass) {
            ramp_start(&st.bphase, st.bs, st.nbs, st.bmag);
        }
        if (st.bphase != RAMP_IDLE && ramp_step(&st.bphase, byp != st.run.bypass, st.bs, st.nbs, st.bmag, RAMP_TAP)) {
            st.run.bypass = byp;
        }
        moving |= st.bphase != RAMP_IDLE || byp != st.run.bypass;
    } else {
        st.run.bypass = byp;
    }
    for (unsigned s = 0; s < ni->nslots; s++) {
        int r = slot_rest(s), m = r < 0 ? -r : r;
        if (ni->slots[s].delay != 255 && st.phase[ni->slots[s].delay] != RAMP_IDLE) {
            m = imin(m, st.smag[s]);
        }
        if (ni->slots[s].byp && st.bphase != RAMP_IDLE) {
            m = imin(m, st.bmag[s]);
        }
        st.fine[nc + s] = (int16_t)(r < 0 ? -m : m);
    }
    for (unsigned k = 0; k < nc + ni->nslots; k++) {
        set_coef(k, st.fine[k]);
    }
    st.moving = (uint8_t)(moving || st.rt_goal);
}

/* An RT LOW / RT MID edit takes curL / curM to the new values (8.4), here
 * a step of 8 (one mid-gain unit) a block, each a recompute. */
static void rt_goals(void)
{
    int l = st.v.ctl[R70_C_RT_LOW], m = st.v.ctl[R70_C_RT_MID];

    if (!st.rt_goal) {
        return;
    }
    st.curl = st.curl < l ? imin(st.curl + 8, l) : imax(st.curl - 8, l);
    st.curm = st.curm < m ? imin(st.curm + 8, m) : imax(st.curm - 8, m);
    st.frozen = st.curm >= 253;
    st.rt_goal = st.curl != l || st.curm != m;
    rates();
    r70_rt(st.a, st.curl, st.curm, st.v.ctl[R70_C_DEFINITION], st.n, st.cur);
    st.n = 1;
}

/* ---- a block of 8 unit samples (10.4) ---- */

#ifdef R70_HOST
static struct r70_run *snap_at, *snap_end;
static int identity;
void r70_identity(int on) { identity = on; }
#endif

static void sub_block(const int32_t *x, int32_t (*y)[2])
{
    if (st.ni->det) {
        apply_rewrites();
    }
    rt_goals();
    ramps();
#ifdef R70_HOST
    if (snap_at && snap_at < snap_end) {
        *snap_at++ = st.run;
    }
#endif
    r70_net_fn[st.v.net](&st.run, x, y, SUB);
#ifdef R70_HOST
    for (unsigned f = 0; identity && f < SUB; f++) {
        y[f][0] = y[f][1] = x[f];
    }
#endif
    walk_run(detector_run(x));
}

/* ---- program load, values ---- */

static void load(const struct r70_values *v)
{
    const struct r70_netinfo *ni;

    st.v = *v;
    st.a = &r70_algos[v->jalgo];
    st.ni = ni = &r70_netinfo[v->net];
    memset(&st.run, 0, sizeof(st.run));
    st.run.bypass = v->ctl[R70_C_DIFFUSION] == 0;
    memcpy(st.cur, v->coef, sizeof(st.cur));
    for (unsigned d = 0; d < R70_DELAYS; d++) {
        st.run.delay[d] = v->delay[d];
    }
    memset(st.owned, 0, sizeof(st.owned));
    memset(st.phase, 0, sizeof(st.phase));
    st.nrd = 0;
    for (unsigned d = 0; d < ni->ndelay; d++) {
        unsigned n = 0;
        for (unsigned s = 0; s < ni->nslots && n < 2; s++) {
            if (ni->slots[s].delay == d) {
                st.rs[st.nrd][n++] = (uint8_t)s;
            }
        }
        if (n) {
            st.rd[st.nrd] = (uint8_t)d;
            st.nrs[st.nrd++] = (uint8_t)n;
        }
    }
    st.nbs = 0;
    st.bphase = RAMP_IDLE;
    for (unsigned s = 0; s < ni->nslots; s++) {
        if (ni->slots[s].byp) {
            st.bs[st.nbs++] = (uint8_t)s;
        }
    }
    if (ni->det) {
        for (unsigned j = 0; j < 2; j++) {
            if (st.a->rtsym[j] != 255) st.owned[st.a->rtsym[j]] = OWN_DETECTOR;
        }
        st.owned[ni->shelf] = OWN_DETECTOR;
        for (unsigned j = 0; j < ni->ninloop; j++) {
            st.owned[ni->inloop[j]] = OWN_DETECTOR;
        }
    }
    for (unsigned j = 0; j < ni->nmods; j++) {
        st.owned[ni->mods[j].k] = OWN_WALK;
    }
    st.k = st.ticks = st.busy = 0;
    detector_reset();
    walk_reset();
    memset(st.fine, 0, sizeof(st.fine));
    for (unsigned k = 0; k < ni->ncoef + ni->nslots; k++) {
        st.scale[k] = (uint8_t)(256 / ni->res[k]);
    }
    st.moving = 1;
    for (unsigned k = 0; k < ni->ncoef; k++) {
        st.fine[k] = (int16_t)(st.cur[k] * scale(k));
    }
    for (unsigned s = 0; s < ni->nslots; s++) {
        st.fine[ni->ncoef + s] = (int16_t)slot_rest(s);
    }
    for (unsigned k = 0; k < R70_COEFS; k++) {
        set_coef(k, st.fine[k]);
    }
    memset(st.in_ring, 0, sizeof(st.in_ring));
    memset(st.out_ring, 0, sizeof(st.out_ring));
    st.in_n = st.p_int = st.p_rem = st.q_rem = st.out_n = 0;
    st.q_int = -LATENCY;
    st.nx = 0;
    st.hp_x[0] = st.hp_x[1] = st.hp_y[0] = st.hp_y[1] = 0.0f;
    st.gain = v->fxgain;
    st.infade = 0.0f;
    st.fade = 1.0f;
    st.restart = 0;
    st.ready = 0;
    st.clearing = 1;
}

static void apply(const struct r70_values *v)
{
    int rt = v->ctl[R70_C_RT_LOW] != st.v.ctl[R70_C_RT_LOW] || v->ctl[R70_C_RT_MID] != st.v.ctl[R70_C_RT_MID];
    int def = v->ctl[R70_C_DEFINITION] != st.v.ctl[R70_C_DEFINITION];

    st.v = *v;
    st.moving = 1;
    if (st.ni->det && rt) {
        st.rt_goal = 1;
        st.flags = 0;
    } else if (st.ni->det && def) {
        rates();
        r70_rt(st.a, st.curl, st.curm, v->ctl[R70_C_DEFINITION], st.n, st.cur);
        st.n = 1;
    }
    walk_config();
}

/* A cleared line reads 4 * 0 + 3 (3.4). */
static int clear_step(void)
{
    uint32_t at = st.clearing - 1u, n = R70_POOL - at < CLEAR_WORDS ? R70_POOL - at : CLEAR_WORDS;
    int32_t *q = r70_pool + at;

#ifdef __ARM_NEON
    const int32x4_t three = vdupq_n_s32(3);
    for (uint32_t i = 0; i < n; i += 4) {
        vst1q_s32(q + i, three);
    }
#else
    for (uint32_t i = 0; i < n; i++) {
        q[i] = 3;
    }
#endif
    st.clearing += n;
    if (st.clearing - 1u >= R70_POOL) {
        st.clearing = 0;
    }
    return 1;
}

/* ---- resamplers ---- */

/* A kernel position (row << 16 | Q16 fraction) for num / den of a sample:
 * num * PHASES * 65536 / den, by a 32 x 32 multiply. */
#define PH_DOWN 338311270ull    /* 2^39 / 1625 */
#define PH_UP   238609294ull    /* 2^39 / 2304 */
static inline uint32_t phase(uint32_t num, uint64_t k) { return (uint32_t)((num * k) >> 16); }

#ifdef __ARM_NEON
static inline float32x4_t frac4(uint32_t q) { return vcvtq_n_f32_u32(vdupq_n_u32(q & 0xFFFFu), 16); }
static inline float32x2_t hsum2(float32x4_t a, float32x4_t b)
{
    return vpadd_f32(vadd_f32(vget_low_f32(a), vget_high_f32(a)), vadd_f32(vget_low_f32(b), vget_high_f32(b)));
}
#endif

/* One unit sample from the bus input: the kernel's rows j and j + 1 over
 * the window, interpolated; a 16-bit sample (10.1). */
static int32_t down(const float *x, uint32_t q)
{
    const float *k0 = r70_kdown[q >> 16], *k1 = k0 + R70_DOWN_TAPS;
#ifdef __ARM_NEON
    float32x4_t a = vdupq_n_f32(0.0f), b = vdupq_n_f32(0.0f);
    for (unsigned i = 0; i < R70_DOWN_TAPS; i += 4) {
        float32x4_t v = vld1q_f32(x + i);
        a = vmlaq_f32(a, vld1q_f32(k0 + i), v);
        b = vmlaq_f32(b, vld1q_f32(k1 + i), v);
    }
    a = vmlaq_f32(a, frac4(q), vsubq_f32(b, a));
    float32x2_t s = vmul_n_f32(hsum2(a, a), 32768.0f);
    uint32x2_t half = vorr_u32(vand_u32(vreinterpret_u32_f32(s), vdup_n_u32(0x80000000u)), vdup_n_u32(0x3F000000u));
    int32x2_t v = vcvt_s32_f32(vadd_f32(s, vreinterpret_f32_u32(half)));
    return vget_lane_s16(vqmovn_s32(vcombine_s32(v, v)), 0);
#else
    float a = 0.0f, b = 0.0f, f = (float)(q & 0xFFFFu) / 65536.0f;
    for (unsigned i = 0; i < R70_DOWN_TAPS; i++) {
        a += k0[i] * x[i];
        b += k1[i] * x[i];
    }
    float v = (a + f * (b - a)) * 32768.0f;
    v += v < 0.0f ? -0.5f : 0.5f;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int32_t)v;
#endif
}

/* One bus frame of both sides from the unit's output rings, x gain. */
static void up(const float *l, const float *r, uint32_t q, float gain, int32_t *out)
{
    const float *k0 = r70_kup[q >> 16], *k1 = k0 + R70_UP_TAPS;
#ifdef __ARM_NEON
    float32x4_t al = vdupq_n_f32(0.0f), bl = al, ar = al, br = al;
    for (unsigned i = 0; i < R70_UP_TAPS; i += 4) {
        float32x4_t c0 = vld1q_f32(k0 + i), c1 = vld1q_f32(k1 + i), vl = vld1q_f32(l + i), vr = vld1q_f32(r + i);
        al = vmlaq_f32(al, c0, vl);
        bl = vmlaq_f32(bl, c1, vl);
        ar = vmlaq_f32(ar, c0, vr);
        br = vmlaq_f32(br, c1, vr);
    }
    float32x4_t f = frac4(q);
    al = vmlaq_f32(al, f, vsubq_f32(bl, al));
    ar = vmlaq_f32(ar, f, vsubq_f32(br, ar));
    vst1_s32(out, vcvt_s32_f32(vmul_n_f32(hsum2(al, ar), gain)));
#else
    float a[2] = { 0, 0 }, b[2] = { 0, 0 }, f = (float)(q & 0xFFFFu) / 65536.0f;
    for (unsigned i = 0; i < R70_UP_TAPS; i++) {
        a[0] += k0[i] * l[i];
        b[0] += k1[i] * l[i];
        a[1] += k0[i] * r[i];
        b[1] += k1[i] * r[i];
    }
    for (int c = 0; c < 2; c++) {
        float v = (a[c] + f * (b[c] - a[c])) * gain;
        out[c] = v >= 2147483520.0f ? 2147483520 : v <= -2147483520.0f ? -2147483520 : (int32_t)v;
    }
#endif
}

/* The network's outputs: AC coupling (10.2) and the FX ADJ gain, into the
 * output rings. */
static void unit_out(int32_t (*y)[2])
{
    st.gain += (st.v.fxgain - st.gain) * GAIN_SMOOTH;
#ifdef __ARM_NEON
    float32x2_t x1 = vld1_f32(st.hp_x), y1 = vld1_f32(st.hp_y);
    for (unsigned f = 0; f < SUB; f++) {
        unsigned i = st.out_n++ & (OUT_RING - 1);
        float32x2_t v = vcvt_f32_s32(vshr_n_s32(vld1_s32(y[f]), 2));
        y1 = vmla_n_f32(vsub_f32(v, x1), y1, HP_A);
        x1 = v;
        float32x2_t g = vmul_n_f32(y1, st.gain);
        vst1_lane_f32(&st.out_ring[0][i], g, 0);
        vst1_lane_f32(&st.out_ring[0][i + OUT_RING], g, 0);
        vst1_lane_f32(&st.out_ring[1][i], g, 1);
        vst1_lane_f32(&st.out_ring[1][i + OUT_RING], g, 1);
    }
    vst1_f32(st.hp_x, x1);
    vst1_f32(st.hp_y, y1);
#else
    for (unsigned f = 0; f < SUB; f++) {
        unsigned i = st.out_n++ & (OUT_RING - 1);
        for (int c = 0; c < 2; c++) {
            float v = (float)(y[f][c] >> 2), h = v - st.hp_x[c] + HP_A * st.hp_y[c];
            st.hp_x[c] = v;
            st.hp_y[c] = h;
            st.out_ring[c][i] = st.out_ring[c][i + OUT_RING] = h * st.gain;
        }
    }
#endif
}

static void take(const int32_t (*send)[2], unsigned count)
{
    int32_t y[SUB][2];

    for (unsigned f = 0; f < count; f++) {
        unsigned i = st.in_n & (IN_RING - 1);
        st.in_ring[i] = st.in_ring[i + IN_RING] = (float)send[f][0] * (SEND_SCALE * st.infade);
        if (st.infade < 1.0f) {
            st.infade = st.infade + INFADE_STEP > 1.0f ? 1.0f : st.infade + INFADE_STEP;
        }
        st.in_n++;
        while (st.p_int + R70_DOWN_TAPS / 2 <= st.in_n - 1) {
            const float *x = st.in_ring + ((st.p_int - R70_DOWN_TAPS / 2 + 1) & (IN_RING - 1));
            st.xq[st.nx++] = 4 * down(x, phase(st.p_rem, PH_DOWN)) + 3;
            st.p_int++;
            st.p_rem += 679;
            if (st.p_rem >= 1625) {
                st.p_rem -= 1625;
                st.p_int++;
            }
            if (st.nx == SUB) {
                st.nx = 0;
                sub_block(st.xq, y);
                unit_out(y);
            }
        }
    }
}

static void give(int32_t (*wet)[2], unsigned count)
{
    for (unsigned f = 0; f < count; f++) {
        const unsigned base = (unsigned)(st.q_int - R70_UP_TAPS / 2 + 1) & (OUT_RING - 1);
        up(st.out_ring[0] + base, st.out_ring[1] + base, phase(st.q_rem, PH_UP), WET_SCALE * st.fade, wet[f]);
        if (st.restart && st.fade > 0.0f) {
            st.fade -= FADE_STEP;
            if (st.fade < 0.0f) st.fade = 0.0f;
        }
        st.q_rem += 1625;
        if (st.q_rem >= 2304) {
            st.q_rem -= 2304;
            st.q_int++;
        }
    }
}

/* ---- audio ---- */

static struct r70_values pending;

/* A program change or a SIZE change while the unit plays: the output
 * fades out (11 ms), then the new program loads from silence (5.9) and its
 * input fades in (21 ms). */
/* Calls may be shorter than 32 frames: program changes, clears and
 * restarts act once per SPAN frames, at a span's first call, as with
 * 32-frame calls (a silent span stays silent). */
#define SPAN 32u

void r70_block(const int32_t (*send)[2], int32_t (*wet)[2], unsigned count)
{
    static unsigned span_f;
    static uint8_t span_mute;
    int first = span_f == 0;

    span_f = (span_f + count) % SPAN;
    if (!first) {
        if (span_mute) {
            goto silent;
        }
        goto run;
    }
    span_mute = 1;
    if (hand.load_seq != st.load_seen) {
        st.load_seen = hand.load_seq;
        st.tgt_seen = hand.tgt_seq;
        st.next = hand.load;
        pending = hand.tgt[hand.tgt_idx];
        if (st.p && !st.clearing) {
            st.restart = 1;
        } else {
            st.p = st.next;
            if (st.p) {
                load(&pending);
            }
        }
    }
    if (!st.p) {
        goto silent;
    }
    if (hand.tgt_seq != st.tgt_seen) {
        const struct r70_values *v = &hand.tgt[hand.tgt_idx];
        st.tgt_seen = hand.tgt_seq;
        if (st.restart || v->ctl[R70_C_SIZE] != st.v.ctl[R70_C_SIZE] || v->net != st.v.net) {
            pending = *v;
            if (!st.restart) {
                st.next = st.p;
            }
            st.restart = 1;
        } else {
            apply(v);
        }
    }
    if (st.clearing && clear_step()) {
        goto silent;
    }
    if (st.restart && st.fade <= 0.0f) {
        st.p = st.next;
        if (st.p) {
            load(&pending);
        }
        goto silent;
    }
    span_mute = 0;
run:
    take(send, count);
    give(wet, count);
    st.ready = 1;
    return;
silent:
    for (unsigned f = 0; f < count; f++) {
        wet[f][0] = wet[f][1] = 0;
    }
}

void r70_flush(void)
{
    st.load_seen = hand.load_seq;
    st.tgt_seen = hand.tgt_seq;
    st.p = st.next = hand.load;
    st.restart = 0;
    if (st.p) {
        pending = hand.tgt[hand.tgt_idx];
        load(&pending);
        while (st.clearing) {
            clear_step();
        }
    }
}

/* ---- foreground ---- */

static struct {
    const struct r70_program *p;
    int16_t edit[R70_EDITS];
    struct r70_values now;
    uint8_t dirty;
} fg;

static void publish(const struct r70_values *v, int loading)
{
    uint8_t w = (uint8_t)(hand.tgt_idx ^ 1u);

    hand.tgt[w] = *v;
    __sync_synchronize();
    hand.tgt_idx = w;
    if (loading) {
        hand.load_seq++;
    } else {
        hand.tgt_seq++;
    }
}

void r70_load(const struct r70_program *p)
{
    fg.p = p;
    for (unsigned e = 0; e < R70_EDITS; e++) {
        fg.edit[e] = -1;
    }
    hand.load = p;
    if (!p) {
        __sync_synchronize();
        hand.load_seq++;
        return;
    }
    fg.dirty = 0;
    r70_build(p, fg.edit, &fg.now);
    publish(&fg.now, 1);
}

void r70_set(unsigned e, int value)
{
    if (e < R70_EDITS && fg.edit[e] != value) {
        fg.edit[e] = (int16_t)value;
        fg.dirty = 1;
    }
}

void r70_poll(void)
{
    struct r70_values v;

    if (!fg.p || !fg.dirty) {
        return;
    }
    fg.dirty = 0;
    r70_build(fg.p, fg.edit, &v);
    if (!memcmp(&v, &fg.now, sizeof(v))) {
        return;
    }
    fg.now = v;
    publish(&v, 0);
}

const struct r70_program *r70_running(void)
{
    return fg.p;
}

int r70_live(unsigned e)
{
    const struct r70_algo *a;

    if (!fg.p || e >= R70_EDITS) {
        return -1;
    }
    a = &r70_algos[fg.p->jalgo];
    for (unsigned f = 0; f < a->nfields; f++) {
        if (a->field_edit[f] == e) {
            unsigned pi = a->field_param[f];
            return fg.now.eff[pi] - a->params[pi].lo;
        }
    }
    return -1;
}

#ifdef R70_HOST
int r70_ready(void)
{
    return st.p && !st.clearing && !st.restart && hand.load_seq == st.load_seen;
}

void r70_detector(int *curl, int *curm, int *n)
{
    *curl = st.curl;
    *curm = st.curm;
    *n = st.n;
}

void r70_native(const int16_t *x, int16_t (*y)[2], unsigned count, struct r70_run *snap, unsigned nsnap)
{
    int32_t xq[SUB], yq[SUB][2];

    if (hand.tgt_seq != st.tgt_seen) {
        st.tgt_seen = hand.tgt_seq;
        apply(&hand.tgt[hand.tgt_idx]);
    }
    snap_at = snap;
    snap_end = snap ? snap + nsnap : 0;
    for (unsigned f0 = 0; f0 + SUB <= count; f0 += SUB) {
        for (unsigned f = 0; f < SUB; f++) {
            xq[f] = 4 * x[f0 + f] + 3;
        }
        sub_block(xq, yq);
        for (unsigned f = 0; f < SUB; f++) {
            y[f0 + f][0] = (int16_t)(yq[f][0] >> 2);
            y[f0 + f][1] = (int16_t)(yq[f][1] >> 2);
        }
    }
    snap_at = snap_end = 0;
}
#endif
