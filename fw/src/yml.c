#include "yml.h"
#include "eq.h"
#include "grains.h"
#include "r70.h"
#include "r80.h"
#include "timelock.h"
#include "voices.h"

#include <stddef.h>
#include <string.h>

enum { SEC_PRESET = 0, SEC_CHAN = 1, SEC_ZONE = 16, SEC_EXTRAS = 255 };
enum { L_BLANK, L_KV, L_SECTION, L_OTHER };
enum { W_PRESET, W_CHAN, W_ZONE, W_IGNORE };
enum { S_PRESET, S_CHANNEL, S_ZONE };

struct line {
    int kind;
    const char *key, *val, *text;
    size_t klen, vlen, tlen;
    int section, number;
};

static size_t slen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int alnum(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int digit(int c)
{
    return c >= '0' && c <= '9';
}

static int ieqn(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

static int ieq(const char *a, size_t n, const char *b)
{
    return slen(b) == n && ieqn(a, b, n);
}

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static void parse_line(const char *s, size_t n, struct line *l)
{
    size_t i = 0, kl, e = n;

    memset(l, 0, sizeof(*l));
    while (i < e && is_ws((unsigned char)s[i])) {
        i++;
    }
    while (e > i && is_ws((unsigned char)s[e - 1])) {
        e--;
    }
    l->text = s + i;
    l->tlen = e - i;
    if (i == e) {
        l->kind = L_BLANK;
        return;
    }
    l->kind = L_OTHER;
    for (kl = 0; i + kl < e && alnum((unsigned char)s[i + kl]); kl++) {
    }
    if (kl == 0 || kl > 32) {
        return;
    }
    l->key = s + i;
    l->klen = kl;
    i += kl;
    while (i < e && is_ws((unsigned char)s[i])) {
        i++;
    }
    if (i < e && s[i] == ':') {
        i++;
        while (i < e && is_ws((unsigned char)s[i])) {
            i++;
        }
        l->kind = L_KV;
        l->val = s + i;
        l->vlen = e - i;
        return;
    }
    if (i < e && digit((unsigned char)s[i])) {
        int num = 0, sec = -1;

        if (ieq(l->key, kl, "Preset")) {
            sec = S_PRESET;
        } else if (ieq(l->key, kl, "Channel")) {
            sec = S_CHANNEL;
        } else if (ieq(l->key, kl, "Zone")) {
            sec = S_ZONE;
        }
        for (; i < e && digit((unsigned char)s[i]); i++) {
            num = num < 100000 ? num * 10 + (s[i] - '0') : num;
        }
        while (i < e && is_ws((unsigned char)s[i])) {
            i++;
        }
        if (sec >= 0 && i < e && s[i] == ':' && i + 1 == e) {
            l->kind = L_SECTION;
            l->section = sec;
            l->number = num;
        }
    }
}

/* Decimal with up to `dec` places, scaled by 10^dec, rounded half away from
 * zero; the whole string must be consumed. */
static int parse_fixed(const char *s, size_t n, int dec, int *out)
{
    size_t i = 0;
    int neg = 0, nd = 0, rem = 0;
    long ip = 0, fp = 0;

    if (i < n && (s[i] == '+' || s[i] == '-')) {
        neg = s[i++] == '-';
    }
    for (; i < n && digit((unsigned char)s[i]); i++, nd++) {
        ip = ip * 10 + (s[i] - '0');
        if (ip > 100000) {
            return 0;
        }
    }
    if (i < n && s[i] == '.') {
        int f = 0;

        i++;
        for (; i < n && digit((unsigned char)s[i]); i++, nd++, f++) {
            if (f < dec) {
                fp = fp * 10 + (s[i] - '0');
            } else if (f == dec) {
                rem = s[i] - '0';
            }
        }
        for (; f < dec; f++) {
            fp *= 10;
        }
    } else {
        for (int f = 0; f < dec; f++) {
            fp *= 10;
        }
    }
    if (i != n || nd == 0) {
        return 0;
    }
    {
        long scale = 1, v;

        for (int f = 0; f < dec; f++) {
            scale *= 10;
        }
        v = ip * scale + fp + (rem >= 5);
        *out = (int)(neg ? -v : v);
    }
    return 1;
}

static int parse_ranged(const char *s, size_t n, int dec, int lo, int hi, int *out)
{
    int v;

    if (!parse_fixed(s, n, dec, &v) || v < lo || v > hi) {
        return 0;
    }
    *out = v;
    return 1;
}

static int parse_int(const char *s, size_t n, int lo, int hi, int *out)
{
    int v;

    if (n == 0 || s[0] == '+' || s[0] == '-' || !parse_fixed(s, n, 0, &v) || v < lo || v > hi) {
        return 0;
    }
    *out = v;
    return 1;
}

/* Unsigned decimal with up to 4 places, as value x 10^4; more places or
 * anything else fails. */
static int parse_u4(const char *s, size_t n, uint64_t *out)
{
    size_t i = 0;
    uint64_t v = 0;
    int nd = 0, f = 0;

    if (i < n && s[i] == '+') {
        i++;
    }
    for (; i < n && digit((unsigned char)s[i]); i++, nd++) {
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v > 1000000000000ull) {
            return 0;
        }
    }
    if (i < n && s[i] == '.') {
        for (i++; i < n && digit((unsigned char)s[i]); i++, nd++, f++) {
            if (f == 4) {
                return 0;
            }
            v = v * 10 + (uint64_t)(s[i] - '0');
        }
    }
    if (i != n || !nd) {
        return 0;
    }
    for (; f < 4; f++) {
        v *= 10;
    }
    *out = v;
    return 1;
}

/* A position in frames (stock writes integers; "123.0000" also read). */
static int parse_pos(const char *s, size_t n, uint32_t *out)
{
    uint64_t v;

    if (!parse_u4(s, n, &v) || v % 10000 || v / 10000 > POS_END) {
        return 0;
    }
    *out = (uint32_t)(v / 10000);
    return 1;
}

/* n / 32 frames as printf("%.4f") prints it, x 10^4 (ties to even). */
static uint64_t len32_u4(uint32_t n)
{
    uint64_t t = (uint64_t)n * 625;

    return t / 2 + ((t & 1) && ((t / 2) & 1));
}

/* Loop length, 1/32 frame; only values that print back the same. */
static int parse_len32(const char *s, size_t n, uint32_t *out)
{
    uint64_t v, q;

    if (!parse_u4(s, n, &v)) {
        return 0;
    }
    if (v >= (uint64_t)(POS_END / 32 + 1) * 10000) {
        *out = POS_END;
        return 1;
    }
    q = (v * 32 + 5000) / 10000;
    if (len32_u4((uint32_t)q) != v) {
        return 0;
    }
    *out = (uint32_t)q;
    return 1;
}

struct tok {
    const char *s;
    size_t n;
};

static int split(const char *s, size_t n, struct tok *t, int max)
{
    int c = 0;
    size_t i = 0;

    while (i < n) {
        while (i < n && is_ws((unsigned char)s[i])) {
            i++;
        }
        if (i == n) {
            break;
        }
        if (c == max) {
            return -1;
        }
        t[c].s = s + i;
        while (i < n && !is_ws((unsigned char)s[i])) {
            i++;
        }
        t[c].n = (size_t)(s + i - t[c].s);
        c++;
    }
    return c;
}

static int is_abc(int c)
{
    c = lower(c);
    return c >= 'a' && c <= 'c';
}

static int parse_src(const char *s, size_t n, int *src)
{
    if (ieq(s, n, "off")) {
        *src = CV_OFF;
    } else if (n == 2 && s[0] == '0' && is_abc((unsigned char)s[1])) {
        *src = CV_REL_A + (lower((unsigned char)s[1]) - 'a');
    } else if (n == 1 && is_abc((unsigned char)s[0])) {
        *src = CV_REL_A + (lower((unsigned char)s[0]) - 'a');
    } else if (n == 3 && ieqn(s, "cv", 2) && is_abc((unsigned char)s[2])) {
        *src = CV_REL_A + (lower((unsigned char)s[2]) - 'a');
    } else if (n == 2 && s[0] >= '1' && s[0] <= '8' && is_abc((unsigned char)s[1])) {
        *src = CV_ABS + (s[0] - '1') * 3 + (lower((unsigned char)s[1]) - 'a');
    } else {
        return 0;
    }
    return 1;
}

static int parse_route(const char *s, size_t n, struct mod *m)
{
    struct tok t[4];
    int c = split(s, n, t, 4), src, amt, a = 1;

    if (c < 2) {
        return 0;
    }
    if (c == 3 && ieq(t[0].s, t[0].n, "cv") && t[1].n == 1) {
        char cv[3] = { 'c', 'v', t[1].s[0] };

        if (!parse_src(cv, 3, &src)) {
            return 0;
        }
        a = 2;
    } else if (c != 2 || !parse_src(t[0].s, t[0].n, &src)) {
        return 0;
    }
    if (!parse_ranged(t[a].s, t[a].n, 2, -AMT_MAX, AMT_MAX, &amt)) {
        return 0;
    }
    m->src = (uint8_t)src;
    m->amt = (int16_t)amt;
    return 1;
}

static int resolve_machine(const char *s, size_t n)
{
    if (ieq(s, n, "off")) {
        return 0;
    }
    for (int i = 1; i < VOICE_COUNT; i++) {
        if (voices[i] && ieq(s, n, voices[i]->name)) {
            return i;
        }
    }
    return -1;
}

static void blank_channel(struct channel_cfg *c, int machine)
{
    const struct voice_desc *v = voices[machine];

    memset(c, 0, sizeof(*c));
    c->machine = (uint8_t)machine;
    c->mix_on = 1;
    c->bits = BITS_MAX;
    for (int d = 0; d < D_COUNT; d++) {
        c->mod[d].amt = AMT_DEFAULT(d);
    }
    sampler_defaults(&c->s);
    channel_fx_defaults(c);
    c->s.play_mode = PLAY_DEFAULT(machine);
    for (unsigned i = 0; i < CONTROLS; i++) {
        c->ctrl[i] = v && i < v->nparams ? (uint8_t)v->params[i].def : 64;
    }
}

void yml_keep_init(struct yml_keep *k, uint32_t cap)
{
    memset(k, 0, sizeof(*k));
    k->cap = cap;
}

static void keep(struct yml_keep *k, int sec, const struct line *l)
{
    if (!k) {
        return;
    }
    if (l->tlen > 255 || k->used + 2 + l->tlen > k->cap) {
        k->lost++;
        return;
    }
    k->pool[k->used++] = (uint8_t)sec;
    k->pool[k->used++] = (uint8_t)l->tlen;
    memcpy(k->pool + k->used, l->text, l->tlen);
    k->used += (uint32_t)l->tlen;
}

static int route_dest(const char *key, size_t n)
{
    static const struct {
        const char *name;
        int dest;
    } t[] = {
        { "PitchCV", D_PITCH }, { "PanMod", D_PAN }, { "MixMod", D_MIX },
        { "BitsMod", D_BITS }, { "MLevelMod", D_LEVEL },
    };

    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (ieq(key, n, t[i].name)) {
            return t[i].dest;
        }
    }
    return -1;
}

static const char *const smod_key[SM_COUNT] = {
    "LinFM", "ExpFM", "PhaseCV", "PMIndexMod", "AliasingMod", "LinAM", "ExpAM",
    "AttackMod", "ReleaseMod", "SampleStartMod", "SampleEndMod", "LoopStartMod", "LoopLengthMod",
    "MStartDelayMod",
};

/* A Stereo/Right channel's offsets (struct channel_cfg ofs), our keys. */
static const struct ofs_key {
    const char *name;
    uint8_t idx, dec;
    int16_t max;
} ofs_keys[] = {
    { "MRightPitch", PO_PITCH, 2, SPITCH_MAX - SPITCH_MIN },
    { "MRightLevel", PO_LEVEL, 1, LEVEL_MAX - MIX_MIN },
    { "MRightMixLevel", PO_MIX, 1, LEVEL_MAX - MIX_MIN },
    { "MRightBits", PO_BITS, 1, BITS_MAX - BITS_MIN },
    { "MRightAttack", PO_ATTACK, 0, ENV_STEPS },
    { "MRightRelease", PO_RELEASE, 0, ENV_STEPS },
    { "MRightAliasing", PO_ALIAS, 0, 100 },
    { "MRightPMIndex", PO_PMINDEX, 2, 100 },
    { "MRightStartDelay", PO_DELAY, 1, SDELAY_MAX },
    { "MRightEQGain1", PO_EQGAIN, 1, 2 * EQ_GAIN_MAX },
    { "MRightEQGain2", PO_EQGAIN + 1, 1, 2 * EQ_GAIN_MAX },
    { "MRightEQGain3", PO_EQGAIN + 2, 1, 2 * EQ_GAIN_MAX },
    { "MRightControl1", PO_CTRL, 0, 127 }, { "MRightControl2", PO_CTRL + 1, 0, 127 },
    { "MRightControl3", PO_CTRL + 2, 0, 127 }, { "MRightControl4", PO_CTRL + 3, 0, 127 },
    { "MRightControl5", PO_CTRL + 4, 0, 127 }, { "MRightControl6", PO_CTRL + 5, 0, 127 },
    { "MRightControl7", PO_CTRL + 6, 0, 127 }, { "MRightControl8", PO_CTRL + 7, 0, 127 },
};
#define OFS_KEYS (sizeof(ofs_keys) / sizeof(ofs_keys[0]))
_Static_assert(OFS_KEYS == PO_COUNT, "an ofs key per offset");

static const struct flag_key {
    const char *name;
    size_t off;
    int max;
} flag_keys[] = {
    { "AttackFromCurrent", offsetof(struct sampler_cfg, attack_from_current), 1 },
    { "Aliasing", offsetof(struct sampler_cfg, aliasing), 100 },
    { "PlayMode", offsetof(struct sampler_cfg, play_mode), 1 },
    { "PMSource", offsetof(struct sampler_cfg, pm_source), 10 },
    { "LoopMode", offsetof(struct sampler_cfg, loop_mode), 2 },
    { "LoopLengthIsEnd", offsetof(struct sampler_cfg, loop_is_end), 1 },
    { "Reverse", offsetof(struct sampler_cfg, reverse), 1 },
    { "LinAMisExtEnv", offsetof(struct sampler_cfg, linam_ext_env), 1 },
    { "SpliceSmoothing", offsetof(struct sampler_cfg, splice), 1 },
    { "ZonesRT", offsetof(struct sampler_cfg, zones_rt), 3 },
    { "MixModIsFader", offsetof(struct sampler_cfg, mixmod_fader), 1 },
};
#define FLAG_KEYS (sizeof(flag_keys) / sizeof(flag_keys[0]))

/* Zone keys; channel-level positions are zone 1's (stock moves them there). */
static int zone_key(struct zone_cfg *z, const struct line *l, int chan_level)
{
    const char *k = l->key, *v = l->val;
    size_t kn = l->klen, vn = l->vlen;
    int x;

    if (ieq(k, kn, "SampleStart")) {
        return parse_pos(v, vn, &z->start);
    }
    if (ieq(k, kn, "SampleEnd")) {
        return parse_pos(v, vn, &z->end);
    }
    if (ieq(k, kn, "LoopStart")) {
        return parse_pos(v, vn, &z->loop_start);
    }
    if (ieq(k, kn, "LoopLength")) {
        return parse_len32(v, vn, &z->loop_len);
    }
    if (chan_level) {
        return 0;
    }
    if (ieq(k, kn, "PitchOffset")) {
        return parse_ranged(v, vn, 2, -9600, 6000, &x) && ((z->pitch = (int16_t)x), 1);
    }
    if (ieq(k, kn, "LevelOffset")) {
        return parse_ranged(v, vn, 1, -900, 60, &x) && ((z->level = (int16_t)x), 1);
    }
    if (ieq(k, kn, "MinVoltage")) {
        return parse_ranged(v, vn, 2, -500, 500, &x) && ((z->minv = (int16_t)x), 1);
    }
    if (ieq(k, kn, "Side")) {
        return parse_int(v, vn, 0, 1, &x) && ((z->side = (uint8_t)x), 1);
    }
    return 0;
}

static int sampler_key(struct sampler_cfg *sc, const struct line *l)
{
    const char *k = l->key, *v = l->val;
    size_t kn = l->klen, vn = l->vlen;
    int x;

    if (ieq(k, kn, "Attack") || ieq(k, kn, "Release")) {
        uint64_t t;

        if (!parse_u4(v, vn, &t) || t > ENV_MAX || (vn && v[0] == '+')) {
            return 0;
        }
        *(ieq(k, kn, "Attack") ? &sc->attack : &sc->release) = (uint32_t)t;
        return 1;
    }
    if (ieq(k, kn, "PMIndex")) {
        return parse_ranged(v, vn, 2, 0, 100, &x) && ((sc->pm_index = (int16_t)x), 1);
    }
    if (ieq(k, kn, "MStartDelay")) {
        return parse_ranged(v, vn, 1, 0, SDELAY_MAX, &x) && ((sc->start_delay = (uint16_t)x), 1);
    }
    if (ieq(k, kn, "XfadeGroup")) {
        int g = vn == 1 ? lower((unsigned char)v[0]) - 'a' : -1;

        return g >= 0 && g < 4 && ((sc->xfade = (uint8_t)(g + 1)), 1);
    }
    if (ieq(k, kn, "ZonesCV") || ieq(k, kn, "MReverseCV")) {
        struct tok t[2];
        int src;

        if (split(v, vn, t, 2) < 1 || !parse_src(t[0].s, t[0].n, &src)) {
            return 0;
        }
        if (ieq(k, kn, "ZonesCV")) {
            sc->zones_cv = (uint8_t)src;
        } else {
            sc->rev_cv = (uint8_t)src;
        }
        return 1;
    }
    for (unsigned i = 0; i < FLAG_KEYS; i++) {
        if (ieq(k, kn, flag_keys[i].name)) {
            return parse_int(v, vn, 0, flag_keys[i].max, &x) &&
                   ((((uint8_t *)sc)[flag_keys[i].off] = (uint8_t)x), 1);
        }
    }
    for (int i = 0; i < SM_COUNT; i++) {
        if (ieq(k, kn, smod_key[i])) {
            return parse_route(v, vn, &sc->mod[i]);
        }
    }
    return zone_key(&sc->zone[0], l, 1);
}

static int chan_key(struct channel_cfg *c, const struct line *l)
{
    const char *k = l->key, *v = l->val;
    size_t kn = l->klen, vn = l->vlen;
    int x, d;

    if (ieq(k, kn, "Pitch")) {
        return parse_ranged(v, vn, 2, -9600, 6000, &x) && ((c->pitch = (int16_t)x), 1);
    }
    if (ieq(k, kn, "Level")) {
        return parse_ranged(v, vn, 1, SLEVEL_MIN, LEVEL_MAX, &x) && ((c->level = (int16_t)x), 1);
    }
    if (ieq(k, kn, "MixLevel")) {
        return parse_ranged(v, vn, 1, MIX_MIN, LEVEL_MAX, &x) && ((c->mix = (int16_t)x), 1);
    }
    if (ieq(k, kn, "Pan")) {
        return parse_ranged(v, vn, 2, -100, 100, &x) && ((c->pan = (int16_t)x), 1);
    }
    if (ieq(k, kn, "Bits") || ieq(k, kn, "BitDepth")) {
        return parse_ranged(v, vn, 1, BITS_MIN, BITS_MAX, &x) && ((c->bits = (int16_t)x), 1);
    }
    if (ieq(k, kn, "ChannelMode")) {
        return parse_int(v, vn, 0, MODE_COUNT - 1, &x) && ((c->mode = (uint8_t)x), 1);
    }
    if (ieq(k, kn, "AutoTrigger")) {
        return parse_int(v, vn, 0, 1, &x) && ((c->auto_trig = (uint8_t)x), 1);
    }
    if (ieq(k, kn, "MMixOn")) {
        return parse_int(v, vn, 0, 1, &x) && ((c->mix_on = (uint8_t)x), 1);
    }
    for (unsigned i = 0; i < OFS_KEYS; i++) {
        if (ieq(k, kn, ofs_keys[i].name)) {
            return parse_ranged(v, vn, ofs_keys[i].dec, -ofs_keys[i].max, ofs_keys[i].max, &x) &&
                   ((c->ofs[ofs_keys[i].idx] = (int16_t)x), 1);
        }
    }
    if (ieq(k, kn, "Machine")) {
        return resolve_machine(v, vn) >= 0;
    }
    if (sampler_key(&c->s, l)) {
        return 1;
    }
    d = route_dest(k, kn);
    if (d >= 0) {
        return parse_route(v, vn, &c->mod[d]);
    }
    if (kn > 7 && ieqn(k, "Machine", 7) && voices[c->machine]) {
        const struct voice_desc *vc = voices[c->machine];

        for (unsigned i = 0; i < vc->nparams && i < CONTROLS; i++) {
            const struct voice_param *pp = &vc->params[i];
            size_t nl = slen(pp->name);

            if (kn == 7 + nl && ieqn(k + 7, pp->name, nl)) {
                return parse_int(v, vn, pp->min, pp->max, &x) && ((c->ctrl[i] = (uint8_t)x), 1);
            }
            if (kn == 7 + nl + 3 && ieqn(k + 7, pp->name, nl) &&
                ieqn(k + 7 + nl, "Mod", 3)) {
                return parse_route(v, vn, &c->mod[D_CTRL + i]);
            }
        }
    }
    return 0;
}

/* The unit's edit slots: their count and yml keys. */
static unsigned reverb_edits(unsigned unit)
{
    return unit == REVERB_70 ? R70_EDITS : unit == REVERB_80 ? R80_EDITS : 0;
}

static const char *reverb_edit_key(unsigned unit, unsigned e)
{
    return unit == REVERB_70 ? r70_edit_key(e) : r80_edit_key(e);
}

/* Reverb<key> : value and Reverb<key>Mod : 1A..8C amount, the unit's edit
 * slots (r70_edit_key, r80_edit_key; ReverbUnit comes first). */
static int reverb_key(struct preset *p, const char *k, size_t kn, const char *v, size_t vn)
{
    for (unsigned e = 0; e < reverb_edits(p->reverb_unit) && e < REVERB_EDITS; e++) {
        const char *name = reverb_edit_key(p->reverb_unit, e);
        size_t nl = slen(name);
        struct tok t[3];
        int x, src;

        if (kn == nl && ieqn(k, name, nl)) {
            return parse_int(v, vn, 0, 32767, &x) && ((p->reverb_edit[e] = (int16_t)x), 1);
        }
        if (kn == nl + 3 && ieqn(k, name, nl) && ieqn(k + nl, "Mod", 3)) {
            if (split(v, vn, t, 3) != 2 || !parse_src(t[0].s, t[0].n, &src) || src < CV_ABS ||
                !parse_ranged(t[1].s, t[1].n, 2, -100, 100, &x)) {
                return 0;
            }
            p->reverb_route[e].source = (signed char)(src - CV_ABS);
            p->reverb_route[e].amount = (signed char)x;
            return 1;
        }
    }
    return 0;
}

static int preset_key(struct preset *p, const struct line *l)
{
    size_t i;

    if (ieq(l->key, l->klen, "Name")) {
        for (i = 0; i < l->vlen && i < PRESET_NAME_LEN; i++) {
            unsigned char ch = (unsigned char)l->val[i];

            p->name[i] = ch >= 32 && ch < 127 ? (char)ch : '?';
        }
        p->name[i] = 0;
        return 1;
    }
    if (ieq(l->key, l->klen, "Data2asCV")) {
        int src;

        if (ieq(l->val, l->vlen, "off")) {
            p->d2cv = 0;
            return 1;
        }
        if (l->vlen == 2 && parse_src(l->val, 2, &src) && src >= CV_ABS) {
            p->d2cv = (uint8_t)(src - CV_ABS + 1);
            return 1;
        }
    }
    if (ieq(l->key, l->klen, "ReverbUnit")) {
        int u;

        return parse_int(l->val, l->vlen, 0, 255, &u) && (u == 0 || u == REVERB_70 || u == REVERB_80) && ((p->reverb_unit = (uint8_t)u), 1);
    }
    if (ieq(l->key, l->klen, "ReverbProgram")) {
        int n;

        return parse_int(l->val, l->vlen, 1, 255, &n) && ((p->reverb_program = (uint8_t)n), 1);
    }
    if (l->klen > 6 && ieqn(l->key, "Reverb", 6)) {
        return reverb_key(p, l->key + 6, l->klen - 6, l->val, l->vlen);
    }
    if (l->klen >= 7 && ieqn(l->key, "Xfade", 5)) {
        int g = lower((unsigned char)l->key[5]) - 'a', src, x;

        if (g < 0 || g >= XFADE_GROUPS) {
            return 0;
        }
        if (ieq(l->key + 6, l->klen - 6, "CV")) {
            if (ieq(l->val, l->vlen, "off")) {
                p->xfade_cv[g] = 0;
                return 1;
            }
            if (l->vlen == 2 && parse_src(l->val, 2, &src) && src >= CV_ABS) {
                p->xfade_cv[g] = (uint8_t)(src - CV_ABS + 1);
                return 1;
            }
        } else if (ieq(l->key + 6, l->klen - 6, "Width")) {
            return parse_ranged(l->val, l->vlen, 2, XFW_MIN, XFW_MAX, &x) && ((p->xfade_width[g] = (int16_t)x), 1);
        }
    }
    return 0;
}

/* Tallent Edition extras (PRESET_FORMAT.md, its src/preset_extras.c):
 * lines appended after the channels, found here in whatever section they
 * end up. Ours (EQ, filter, Grain, MixFX) are read into the preset; the rest, and
 * lines of ours with another number count, are kept for the end of the
 * file. */
#define TA_EQ_NUMBERS       11
#define TA_FILTER_NUMBERS   4
#define TA_NUMBERS_MAX      16
#define TA_CENTS_PER_UNIT   100
#define TA_CENTS_PER_STEP   10
#define TA_EQ_DECIMALS      ((1u << 2) | (1u << 5) | (1u << 8))
#define TA_FILTER_DECIMALS  (1u << 1)
#define TA_EXTRAS_FORMAT    3
#define TA_MIXFX_LINE       16

enum { TA_NONE, TA_FORMAT, TA_OURS, TA_OTHER };

static int ta_kind(const char *k, size_t n)
{
    if (ieq(k, n, "ExtrasFormat")) {
        return TA_FORMAT;
    }
    if ((n > 2 && ieqn(k, "EQ", 2) && (digit((unsigned char)k[2]) || (n > 4 && ieqn(k + 2, "CV", 2)))) ||
        (n > 4 && ieqn(k, "FILT", 4) && (digit((unsigned char)k[4]) || (n > 5 && ieqn(k + 4, "CV", 2)) || lower(k[4]) == 'r'))) {
        return TA_OURS;
    }
    if (n > 5 && ieqn(k, "MixFX", 5)) {
        return TA_OURS;
    }
    if (n >= 5 && ieqn(k, "Grain", 5)) {
        return TA_OURS;
    }
    if (n > 8 && ieqn(k, "TimeLock", 8)) {
        return TA_OURS;
    }
    return TA_NONE;
}

static int ta_clamp(int x, int lo, int hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

/* A signed decimal after spaces; 0 when there is none. */
static const char *ta_number(const char *s, const char *e, int *out)
{
    int neg, v = 0, nd = 0;

    while (s < e && (*s == ' ' || *s == '\t')) {
        s++;
    }
    neg = s < e && *s == '-';
    s += neg;
    for (; s < e && digit((unsigned char)*s); s++, nd++) {
        v = v < 100000000 ? v * 10 + (*s - '0') : v;
    }
    if (!nd) {
        return 0;
    }
    *out = neg ? -v : v;
    return s;
}

/* Up to two decimals (more are dropped), in hundredths. */
static const char *ta_hundredths(const char *s, const char *e, int *out)
{
    int whole, frac = 0, scale = TA_CENTS_PER_UNIT;

    if (!(s = ta_number(s, e, &whole))) {
        return 0;
    }
    if (s < e && *s == '.') {
        for (s++; s < e && digit((unsigned char)*s); s++) {
            if (scale /= 10) {
                frac += (*s - '0') * scale;
            }
        }
    }
    *out = whole * TA_CENTS_PER_UNIT + (whole < 0 ? -frac : frac);
    return s;
}

static void ta_routes(int8_t *src, int8_t *amt, const int *n, int routes)
{
    for (int r = 0; r < routes; r++) {
        src[r] = (int8_t)ta_clamp(n[2 * r], TA_SRC_OFF, EQ_CV_SOURCE_MAX);
        amt[r] = (int8_t)ta_clamp(n[2 * r + 1], -EQ_CV_AMOUNT_MAX, EQ_CV_AMOUNT_MAX);
    }
}

/* MixFX<n>: sixteen bytes of the mix effects' settings from 16(n - 1). */
static int ta_mixfx(struct preset *p, const struct line *l)
{
    const char *k = l->key + 5, *ke = l->key + l->klen, *v = l->val, *ve = l->val + l->vlen;
    int chunk = 0, count = 0, n[TA_MIXFX_LINE], more;

    for (; k < ke && digit((unsigned char)*k) && chunk <= TA_MIXFX_BYTES; k++) {
        chunk = chunk * 10 + (*k - '0');
    }
    if (k != ke || chunk < 1 || chunk > TA_MIXFX_BYTES / TA_MIXFX_LINE) {
        return 0;
    }
    while (count < TA_MIXFX_LINE && (v = ta_number(v, ve, &n[count]))) {
        count++;
    }
    if (count != TA_MIXFX_LINE || ta_number(v, ve, &more)) {
        return 0;
    }
    while (v < ve && (*v == ' ' || *v == '\t')) {
        v++;
    }
    if (v != ve) {
        return 0;
    }
    for (int i = 0; i < TA_MIXFX_LINE; i++) {
        p->mixfx[(chunk - 1) * TA_MIXFX_LINE + i] = (uint8_t)ta_clamp(n[i], 0, 255);
    }
    return 1;
}

/* Grain<c>, GrainCV<c>; Grain and GrainCV (format 1) for every channel,
 * Grain's first number then the playing channel (0 none). */
static int ta_grain(struct preset *p, const struct line *l)
{
    const char *k = l->key + 5, *ke = l->key + l->klen, *v = l->val, *ve = l->val + l->vlen;
    int cv = ke - k >= 2 && ieqn(k, "CV", 2), ch = -1, count = 0, n[TA_NUMBERS_MAX], more;

    k += cv ? 2 : 0;
    if (k < ke) {
        if (*k < '1' || *k > '8' || k + 1 != ke) {
            return 0;
        }
        ch = *k - '1';
    }
    int wanted = cv ? 2 * TA_GRAIN_ROUTES : TA_GRAIN_FIELDS;
    while (count < wanted && (v = ta_number(v, ve, &n[count]))) {
        count++;
    }
    if (count != wanted || ta_number(v, ve, &more)) {
        return 0;
    }
    while (v < ve && (*v == ' ' || *v == '\t')) {
        v++;
    }
    if (v != ve) {
        return 0;
    }
    for (int c = ch < 0 ? 0 : ch; c < (ch < 0 ? CHANNELS : ch + 1); c++) {
        struct grain_value *g = &p->ch[c].grain;

        if (cv) {
            ta_routes(g->source, g->amount, n, TA_GRAIN_ROUTES);
            continue;
        }
        for (int f = 0; f < TA_GRAIN_FIELDS; f++) {
            g->value[f] = (int16_t)ta_clamp(n[f], grain_ranges[f].low, grain_ranges[f].high);
        }
        if (ch < 0) {
            g->value[GRAIN_ON] = n[GRAIN_ON] == c + 1;
        }
    }
    return 1;
}

/* TimeLock<c> : on [tempo speed input pulses], absent numbers 0 (input
 * Off); TimeLock<c>Z<z> : beats. Clamped as Tallent's reader. */
static int ta_timelock(struct preset *p, const struct line *l)
{
    const char *k = l->key + 8, *ke = l->key + l->klen, *v = l->val, *ve = l->val + l->vlen;
    int ch, zone = -1, count = 0, n[5] = { 0, 0, 0, TA_SRC_OFF, 0 }, more;

    if (k >= ke || *k < '1' || *k > '8') {
        return 0;
    }
    ch = *k++ - '1';
    if (k < ke && lower(*k) == 'z') {
        if (k + 2 != ke || k[1] < '1' || k[1] > '8') {
            return 0;
        }
        zone = k[1] - '1';
        k += 2;
    }
    if (k != ke) {
        return 0;
    }
    int wanted = zone < 0 ? 5 : 1;
    for (const char *next; count < wanted && (next = ta_number(v, ve, &n[count])); v = next) {
        count++;
    }
    if (!count || ta_number(v, ve, &more)) {
        return 0;
    }
    while (v < ve && (*v == ' ' || *v == '\t')) {
        v++;
    }
    if (v != ve) {
        return 0;
    }
    if (zone >= 0) {
        p->ch[ch].s.zone[zone].beats = (uint8_t)ta_clamp(n[0], 0, TIME_LOCK_BEATS_MAX);
        return 1;
    }
    struct timelock_value *t = &p->ch[ch].tl;
    t->on = n[0] != 0;
    t->tempo = (uint8_t)ta_clamp(n[1], 0, TIME_LOCK_TEMPOS - 1);
    t->speed = (int16_t)ta_clamp(n[2], TIME_LOCK_SPEED_MIN, TIME_LOCK_SPEED_MAX);
    t->input = (int8_t)ta_clamp(n[3], TA_SRC_OFF, EQ_CV_SOURCE_MAX);
    t->ppqn = (uint8_t)ta_clamp(n[4], 0, TIME_LOCK_PPQN_MAX);
    return 1;
}

/* One of our lines; 0 if it isn't one we take (then kept). */
static int ta_line(struct preset *p, const struct line *l)
{
    const char *k = l->key, *ke = l->key + l->klen, *v = l->val, *ve = l->val + l->vlen;
    int filter = lower(k[0]) == 'f', cv, right, ch, zone = -1, wanted, count = 0, n[TA_NUMBERS_MAX], more;
    unsigned dec;

    if (lower(k[0]) == 'm') {
        return ta_mixfx(p, l);
    }
    if (lower(k[0]) == 'g') {
        return ta_grain(p, l);
    }
    if (lower(k[0]) == 't') {
        return ta_timelock(p, l);
    }

    k += filter ? 4 : 2;
    cv = ke - k >= 2 && ieqn(k, "CV", 2);
    k += cv ? 2 : 0;
    right = filter && !cv && k < ke && lower(*k) == 'r';
    k += right;
    if (k >= ke || *k < '1' || *k > '8') {
        return 0;
    }
    ch = *k++ - '1';
    if (k < ke && lower(*k) == 'z') {
        if (k + 2 != ke || k[1] < '1' || k[1] > '8') {
            return 0;
        }
        zone = k[1] - '1';
        k += 2;
    }
    if (k != ke) {
        return 0;
    }
    wanted = right ? 1 : cv ? 2 * (filter ? TA_FILTER_MODS : TA_EQ_MODS) : filter ? TA_FILTER_NUMBERS : TA_EQ_NUMBERS;
    dec = cv || right ? 0 : filter ? TA_FILTER_DECIMALS : TA_EQ_DECIMALS;
    while (count < wanted && (v = (dec >> count) & 1 ? ta_hundredths(v, ve, &n[count]) : ta_number(v, ve, &n[count]))) {
        count++;
    }
    if (count != wanted || ta_number(v, ve, &more)) {
        return 0;
    }
    while (v < ve && (*v == ' ' || *v == '\t')) {
        v++;
    }
    if (v != ve) {
        return 0;
    }
    if (filter) {
        struct filter_value *f = zone < 0 ? &p->ch[ch].filter : &p->ch[ch].s.zone[zone].filter;

        if (zone >= 0 && !f->override) {
            filter_value_defaults(f);
            f->override = 1;
        }
        if (right) {
            int c = ta_clamp(n[0], -EQ_STEP_MAX * TA_CENTS_PER_STEP, EQ_STEP_MAX * TA_CENTS_PER_STEP);
            f->right = (int16_t)((c + (c < 0 ? -TA_CENTS_PER_STEP / 2 : TA_CENTS_PER_STEP / 2)) / TA_CENTS_PER_STEP);
        } else if (cv) {
            ta_routes(f->source, f->amount, n, TA_FILTER_MODS);
        } else {
            f->type = (uint8_t)ta_clamp(n[0], FILTER_OFF, FILTER_TYPES - 1);
            f->cutoff = (uint16_t)eq_step_nearest((u32)ta_clamp(n[1], 0, EQ_FREQUENCY_MAX * TA_CENTS_PER_UNIT));
            f->resonance = (uint8_t)ta_clamp(n[2], 0, FILTER_RESONANCE_MAX);
            f->extra = (uint8_t)ta_clamp(n[3], 0, FILTER_EXTRA_MAX);
        }
        return 1;
    }
    struct eq_value *e = zone < 0 ? &p->ch[ch].eq : &p->ch[ch].s.zone[zone].eq;

    if (zone >= 0 && !e->override) {
        eq_value_defaults(e);
        e->override = 1;
    }
    if (cv) {
        ta_routes(e->source, e->amount, n, TA_EQ_MODS);
        return 1;
    }
    e->on = n[0] != 0;
    e->hpf = (uint16_t)(n[1] ? ta_clamp(n[1], EQ_HPF_MIN, EQ_HPF_MAX) : 0);
    for (int b = 0; b < 3; b++) {
        e->frequency[b] = (uint16_t)eq_step_nearest((u32)ta_clamp(n[2 + 3 * b], 0, EQ_FREQUENCY_MAX * TA_CENTS_PER_UNIT));
        e->gain[b] = (int16_t)ta_clamp(n[3 + 3 * b], -EQ_GAIN_MAX, EQ_GAIN_MAX);
        e->q[b] = (uint8_t)ta_clamp(n[4 + 3 * b], EQ_Q_OWN, EQ_Q_STEPS);
    }
    eq_limit(e);
    return 1;
}

int yml_parse(const char *buf, size_t len, struct preset *p, struct yml_keep *k)
{
    int recognised = 0;

    if (!buf || !p) {
        return YML_E_ARG;
    }
    if (k) {
        yml_keep_init(k, k->cap);
    }
    memset(p, 0, sizeof(*p));
    p->magic = PRESET_MAGIC;
    p->version = PRESET_VERSION;
    for (int c = 0; c < CHANNELS; c++) {
        blank_channel(&p->ch[c], 0);
    }
    for (int g = 0; g < XFADE_GROUPS; g++) {
        p->xfade_width[g] = XFW_DEFAULT;
    }
    for (int e = 0; e < REVERB_EDITS; e++) {
        p->reverb_edit[e] = -1;
        p->reverb_route[e].source = TA_SRC_OFF;
    }

    for (int pass = 0; pass < 2; pass++) {
        int where = W_PRESET, ch = 0, zone = 0;
        size_t pos = 0;

        while (pos < len) {
            const char *s = buf + pos;
            size_t n = 0;
            int nl;
            struct line l;

            while (n < len - pos && s[n] != '\n') {
                n++;
            }
            nl = n < len - pos;
            pos += n + (size_t)nl;
            parse_line(s, n, &l);
            if (l.kind == L_BLANK) {
                continue;
            }
            if (l.kind == L_SECTION) {
                recognised = 1;
                if (l.section == S_PRESET) {
                    where = W_PRESET;
                } else if (l.section == S_CHANNEL) {
                    if (l.number >= 1 && l.number <= CHANNELS) {
                        where = W_CHAN;
                        ch = l.number - 1;
                        if (k && pass) {
                            k->chan_seen |= (uint8_t)(1u << ch);
                        }
                    } else {
                        where = W_IGNORE;
                    }
                } else if (where == W_CHAN || where == W_ZONE) {
                    if (l.number >= 1 && l.number <= 8) {
                        where = W_ZONE;
                        zone = l.number - 1;
                        if (k && pass) {
                            k->zone_seen[ch] |= (uint8_t)(1u << zone);
                        }
                    } else {
                        where = W_IGNORE;
                    }
                } else {
                    where = W_IGNORE;
                }
                continue;
            }
            if (pass == 0) {
                if (where == W_CHAN && l.kind == L_KV && ieq(l.key, l.klen, "Machine")) {
                    int m = resolve_machine(l.val, l.vlen);

                    if (m >= 0) {
                        p->ch[ch].machine = (uint8_t)m;
                    }
                }
                continue;
            }
            if (l.kind == L_KV && ta_kind(l.key, l.klen)) {
                int t = ta_kind(l.key, l.klen);

                if (t == TA_OURS && ta_line(p, &l)) {
                    recognised = 1;
                } else if (t != TA_FORMAT) {
                    keep(k, SEC_EXTRAS, &l);
                }
            } else if (where == W_IGNORE) {
                if (k) {
                    k->lost++;
                }
            } else if (where == W_ZONE) {
                if (l.kind == L_KV && zone_key(&p->ch[ch].s.zone[zone], &l, 0)) {
                    recognised = 1;
                } else {
                    keep(k, SEC_ZONE + ch * 8 + zone, &l);
                }
            } else if (where == W_CHAN) {
                if (l.kind == L_KV && chan_key(&p->ch[ch], &l)) {
                    recognised = 1;
                } else {
                    keep(k, SEC_CHAN + ch, &l);
                }
            } else if (l.kind == L_KV && preset_key(p, &l)) {
                recognised = 1;
            } else {
                keep(k, SEC_PRESET, &l);
            }
        }
        if (pass == 0) {
            for (int c = 0; c < CHANNELS; c++) {
                blank_channel(&p->ch[c], p->ch[c].machine);
            }
        }
    }
    return recognised ? YML_OK : YML_E_FORMAT;
}

int yml_samples(const struct yml_keep *k, struct yml_samples *s)
{
    uint32_t i = 0;
    int found = 0;

    memset(s, 0, sizeof(*s));
    while (k && i + 2 <= k->used) {
        uint32_t sec = k->pool[i], n = k->pool[i + 1];
        struct line l;

        if (i + 2 + n > k->used) {
            break;
        }
        if (sec >= SEC_ZONE && sec < SEC_ZONE + CHANNELS * YML_ZONES) {
            parse_line((const char *)k->pool + i + 2, n, &l);
            if (l.kind == L_KV && ieq(l.key, l.klen, "Sample") && l.vlen && l.vlen <= YML_SAMPLE_LEN) {
                char *d = s->name[(sec - SEC_ZONE) / YML_ZONES][(sec - SEC_ZONE) % YML_ZONES];

                found += !d[0];
                memcpy(d, l.val, l.vlen);
                d[l.vlen] = 0;
            }
        }
        i += 2 + n;
    }
    return found;
}

struct wr {
    char *out;
    size_t cap, len;
};

static void putc_(struct wr *w, char c)
{
    if (w->len < w->cap) {
        w->out[w->len] = c;
    }
    w->len++;
}

static void puts_(struct wr *w, const char *s)
{
    while (*s) {
        putc_(w, *s++);
    }
}

static void indent(struct wr *w, int n)
{
    while (n--) {
        putc_(w, ' ');
    }
}

/* printf("%[+]0<width>.<dec>f") of v / 10^dec. */
static void put_fixed(struct wr *w, int v, int dec, int plus, int width)
{
    char t[16];
    int n = 0, sg = v < 0 ? '-' : plus ? '+' : 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;

    for (int i = 0; i < dec; i++) {
        t[n++] = (char)('0' + u % 10);
        u /= 10;
    }
    if (dec) {
        t[n++] = '.';
    }
    do {
        t[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (sg) {
        putc_(w, (char)sg);
    }
    for (int i = n + (sg != 0); i < width; i++) {
        putc_(w, '0');
    }
    while (n) {
        putc_(w, t[--n]);
    }
}

/* printf("%<width>.4f") of v / 10^4. */
static void put_u4(struct wr *w, uint64_t v, int width)
{
    char t[24];
    int n = 0;

    for (int i = 0; i < 4; i++) {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    t[n++] = '.';
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (int i = n; i < width; i++) {
        putc_(w, ' ');
    }
    while (n) {
        putc_(w, t[--n]);
    }
}

static void put_src(struct wr *w, int src)
{
    if (src == CV_OFF) {
        puts_(w, "Off");
    } else if (src < CV_ABS) {
        putc_(w, '0');
        putc_(w, (char)('A' + src - CV_REL_A));
    } else {
        src -= CV_ABS;
        putc_(w, (char)('1' + src / 3));
        putc_(w, (char)('A' + src % 3));
    }
}

static void put_route(struct wr *w, int ind, const char *key, const char *mid, const char *suffix,
                      const struct mod *m)
{
    if (m->src == CV_OFF) {
        return;
    }
    indent(w, ind);
    puts_(w, key);
    puts_(w, mid);
    puts_(w, suffix);
    puts_(w, " : ");
    put_src(w, m->src);
    putc_(w, ' ');
    put_fixed(w, m->amt, 2, 0, 4);
    putc_(w, '\n');
}

static void put_kv(struct wr *w, int ind, const char *key, int v, int dec, int plus, int width)
{
    indent(w, ind);
    puts_(w, key);
    puts_(w, " : ");
    put_fixed(w, v, dec, plus, width);
    putc_(w, '\n');
}

static void put_key(struct wr *w, int ind, const char *key)
{
    indent(w, ind);
    puts_(w, key);
    puts_(w, " : ");
}

/* Stock's formats: positions "%d", loop length "%11.4f". */
static void put_zone(struct wr *w, const struct zone_cfg *z)
{
    if (z->start) {
        put_kv(w, 6, "SampleStart", (int)z->start, 0, 0, 0);
    }
    if (z->end != POS_END) {
        put_kv(w, 6, "SampleEnd", (int)z->end, 0, 0, 0);
    }
    if (z->loop_start) {
        put_kv(w, 6, "LoopStart", (int)z->loop_start, 0, 0, 0);
    }
    if (z->loop_len != POS_END) {
        put_key(w, 6, "LoopLength");
        put_u4(w, len32_u4(z->loop_len), 11);
        putc_(w, '\n');
    }
    if (z->side) {
        put_kv(w, 6, "Side", z->side, 0, 0, 0);
    }
    if (z->pitch) {
        put_kv(w, 6, "PitchOffset", z->pitch, 2, 1, 4);
    }
    if (z->level) {
        put_kv(w, 6, "LevelOffset", z->level, 1, 1, 4);
    }
    if (z->minv) {
        put_kv(w, 6, "MinVoltage", z->minv, 2, 1, 0);
    }
}

static void put_sampler(struct wr *w, const struct sampler_cfg *s, int machine)
{
    if (s->attack) {
        put_key(w, 4, "Attack");
        put_u4(w, s->attack, 7);
        putc_(w, '\n');
    }
    if (s->release != 1000) {
        put_key(w, 4, "Release");
        put_u4(w, s->release, 7);
        putc_(w, '\n');
    }
    if (s->pm_index) {
        put_kv(w, 4, "PMIndex", s->pm_index, 2, 0, 4);
    }
    if (s->start_delay) {
        put_kv(w, 4, "MStartDelay", s->start_delay, 1, 0, 0);
    }
    for (unsigned i = 0; i < FLAG_KEYS; i++) {
        int v = ((const uint8_t *)s)[flag_keys[i].off];
        int def = flag_keys[i].off == offsetof(struct sampler_cfg, play_mode) ? PLAY_DEFAULT(machine) : 0;

        if (v != def) {
            put_kv(w, 4, flag_keys[i].name, v, 0, 0, 0);
        }
    }
    if (s->xfade) {
        put_key(w, 4, "XfadeGroup");
        putc_(w, (char)('A' + s->xfade - 1));
        putc_(w, '\n');
    }
    for (int i = 0; i < SM_COUNT; i++) {
        put_route(w, 4, smod_key[i], "", "", &s->mod[i]);
    }
    if (s->zones_cv != CV_OFF) {
        put_key(w, 4, "ZonesCV");
        put_src(w, s->zones_cv);
        putc_(w, '\n');
    }
    if (s->rev_cv != CV_OFF) {
        put_key(w, 4, "MReverseCV");
        put_src(w, s->rev_cv);
        putc_(w, '\n');
    }
}

const char *(*yml_sample_name)(unsigned smp);

static void put_kept(struct wr *w, const struct yml_keep *k, int sec, int ind)
{
    uint32_t i = 0;

    while (k && i + 2 <= k->used) {
        uint32_t n = k->pool[i + 1];
        struct line l;

        if (k->pool[i] == sec && i + 2 + n <= k->used && yml_sample_name && sec >= SEC_ZONE) {
            parse_line((const char *)k->pool + i + 2, n, &l);
            if (l.kind == L_KV && ieq(l.key, l.klen, "Sample")) {
                i += 2 + n;
                continue;
            }
        }
        if (k->pool[i] == sec && i + 2 + n <= k->used) {
            indent(w, ind);
            for (uint32_t j = 0; j < n; j++) {
                putc_(w, (char)k->pool[i + 2 + j]);
            }
            putc_(w, '\n');
        }
        i += 2 + n;
    }
}

static void put_number(struct wr *w, const char *what, int n)
{
    puts_(w, what);
    putc_(w, ' ');
    put_fixed(w, n, 0, 0, 0);
    puts_(w, " :\n");
}

static void put_channel(struct wr *w, const struct preset *p, const struct yml_keep *k, int c)
{
    const struct channel_cfg *ch = &p->ch[c];
    const struct voice_desc *v = ch->machine < VOICE_COUNT ? voices[ch->machine] : 0;
    size_t start = w->len;
    size_t body;

    indent(w, 2);
    put_number(w, "Channel", c + 1);
    body = w->len;
    put_kept(w, k, SEC_CHAN + c, 4);
    if (ch->pitch) {
        put_kv(w, 4, "Pitch", ch->pitch, 2, 1, 4);
    }
    if (ch->level) {
        put_kv(w, 4, "Level", ch->level, 1, 1, 4);
    }
    if (ch->pan) {
        put_kv(w, 4, "Pan", ch->pan, 2, 1, 3);
    }
    if (ch->bits != BITS_MAX) {
        put_kv(w, 4, "Bits", ch->bits, 1, 0, 4);
    }
    if (ch->mix) {
        put_kv(w, 4, "MixLevel", ch->mix, 1, 1, 4);
    }
    if (ch->mode) {
        put_kv(w, 4, "ChannelMode", ch->mode, 0, 0, 0);
    }
    if (ch->auto_trig) {
        put_kv(w, 4, "AutoTrigger", 1, 0, 0, 0);
    }
    put_route(w, 4, "PitchCV", "", "", &ch->mod[D_PITCH]);
    put_route(w, 4, "BitsMod", "", "", &ch->mod[D_BITS]);
    put_route(w, 4, "PanMod", "", "", &ch->mod[D_PAN]);
    put_route(w, 4, "MixMod", "", "", &ch->mod[D_MIX]);
    put_route(w, 4, "MLevelMod", "", "", &ch->mod[D_LEVEL]);
    put_sampler(w, &ch->s, ch->machine);
    if (!ch->mix_on) {
        put_kv(w, 4, "MMixOn", 0, 0, 0, 0);
    }
    for (unsigned i = 0; i < OFS_KEYS; i++) {
        if (ch->ofs[ofs_keys[i].idx]) {
            put_kv(w, 4, ofs_keys[i].name, ch->ofs[ofs_keys[i].idx], ofs_keys[i].dec, 1, 0);
        }
    }
    if (v) {
        indent(w, 4);
        puts_(w, "Machine : ");
        puts_(w, v->name);
        putc_(w, '\n');
        for (unsigned i = 0; i < v->nparams && i < CONTROLS; i++) {
            indent(w, 4);
            puts_(w, "Machine");
            puts_(w, v->params[i].name);
            puts_(w, " : ");
            put_fixed(w, ch->ctrl[i], 0, 0, 0);
            putc_(w, '\n');
            put_route(w, 4, "Machine", v->params[i].name, "Mod", &ch->mod[D_CTRL + i]);
        }
    }
    for (int z = 0; z < 8; z++) {
        size_t zs = w->len;

        const struct zone_cfg *zc = &ch->s.zone[z];
        const char *sn = yml_sample_name && zc->smp ? yml_sample_name(zc->smp) : 0;

        if (yml_sample_name && !sn) {
            continue;
        }
        indent(w, 4);
        put_number(w, "Zone", z + 1);
        {
            size_t zb = w->len;

            if (sn) {
                put_key(w, 6, "Sample");
                puts_(w, sn);
                putc_(w, '\n');
            }
            put_kept(w, k, SEC_ZONE + c * 8 + z, 6);
            put_zone(w, zc);
            if (w->len == zb && !(k && (k->zone_seen[c] >> z & 1))) {
                w->len = zs;
            }
        }
    }
    if (w->len == body && !(k && (k->chan_seen >> c & 1))) {
        w->len = start;
    }
}

static int ta_written;

/* A Tallent line: decimals marks frequencies in hundredths of a hertz,
 * written whole when that reads back as the same step. */
static void ta_put(struct wr *w, const char *key, int ch, int zone, const int *n, int count, unsigned decimals)
{
    if (!ta_written) {
        ta_written = 1;
        put_kv(w, 2, "ExtrasFormat", TA_EXTRAS_FORMAT, 0, 0, 0);
    }
    indent(w, 2);
    puts_(w, key);
    putc_(w, (char)('1' + ch));
    if (zone >= 0) {
        putc_(w, 'Z');
        putc_(w, (char)('1' + zone));
    }
    puts_(w, " :");
    for (int i = 0; i < count; i++) {
        int whole = (n[i] + TA_CENTS_PER_UNIT / 2) / TA_CENTS_PER_UNIT;

        putc_(w, ' ');
        if (!((decimals >> i) & 1)) {
            put_fixed(w, n[i], 0, 0, 0);
        } else if (eq_step_nearest((u32)(whole * TA_CENTS_PER_UNIT)) == eq_step_nearest((u32)n[i])) {
            put_fixed(w, whole, 0, 0, 0);
        } else {
            put_fixed(w, n[i], 2, 0, 0);
        }
    }
    putc_(w, '\n');
}

static int ta_routes_out(const int8_t *src, const int8_t *amt, int *n, int routes)
{
    int set = 0;

    for (int r = 0; r < routes; r++) {
        n[2 * r] = src[r];
        n[2 * r + 1] = amt[r];
        set |= src[r] != TA_SRC_OFF || amt[r] != EQ_CV_AMOUNT_MAX;
    }
    return set;
}

static void ta_eq_numbers(const struct eq_value *e, int *n)
{
    n[0] = e->on;
    n[1] = e->hpf;
    for (int b = 0; b < 3; b++) {
        n[2 + 3 * b] = (int)eq_step_centihertz(e->frequency[b]);
        n[3 + 3 * b] = e->gain[b];
        n[4 + 3 * b] = e->q[b];
    }
}

static void ta_filter_numbers(const struct filter_value *f, int *n)
{
    n[0] = f->type;
    n[1] = (int)eq_step_centihertz(f->cutoff);
    n[2] = f->resonance;
    n[3] = f->extra;
}

/* A channel's records are written when they differ from the defaults; a
 * zone with its own always has its first line. */
static void ta_put_eq(struct wr *w, const struct eq_value *e, int ch, int zone)
{
    static struct eq_value plain;
    int n[TA_NUMBERS_MAX], std[TA_EQ_NUMBERS];

    eq_value_defaults(&plain);
    ta_eq_numbers(&plain, std);
    ta_eq_numbers(e, n);
    if (zone >= 0 || memcmp(n, std, sizeof(std))) {
        ta_put(w, "EQ", ch, zone, n, TA_EQ_NUMBERS, TA_EQ_DECIMALS);
    }
    if (ta_routes_out(e->source, e->amount, n, TA_EQ_MODS)) {
        ta_put(w, "EQCV", ch, zone, n, 2 * TA_EQ_MODS, 0);
    }
}

static void ta_put_filter(struct wr *w, const struct filter_value *f, int ch, int zone)
{
    static struct filter_value plain;
    int n[TA_NUMBERS_MAX], std[TA_FILTER_NUMBERS];

    filter_value_defaults(&plain);
    ta_filter_numbers(&plain, std);
    ta_filter_numbers(f, n);
    if (zone >= 0 || memcmp(n, std, sizeof(std))) {
        ta_put(w, "FILT", ch, zone, n, TA_FILTER_NUMBERS, TA_FILTER_DECIMALS);
    }
    if (ta_routes_out(f->source, f->amount, n, TA_FILTER_MODS)) {
        ta_put(w, "FILTCV", ch, zone, n, 2 * TA_FILTER_MODS, 0);
    }
    if (f->right) {
        n[0] = f->right * TA_CENTS_PER_STEP;
        ta_put(w, "FILTR", ch, zone, n, 1, 0);
    }
}

static void ta_put_all(struct wr *w, const struct preset *p, const struct yml_keep *k)
{
    ta_written = 0;
    for (int c = 0; c < CHANNELS; c++) {
        const struct channel_cfg *ch = &p->ch[c];

        ta_put_eq(w, &ch->eq, c, -1);
        for (int z = 0; z < ZONES; z++) {
            if (ch->s.zone[z].eq.override) {
                ta_put_eq(w, &ch->s.zone[z].eq, c, z);
            }
        }
        ta_put_filter(w, &ch->filter, c, -1);
        for (int z = 0; z < ZONES; z++) {
            if (ch->s.zone[z].filter.override) {
                ta_put_filter(w, &ch->s.zone[z].filter, c, z);
            }
        }
    }
    /* Then each channel's grain lines, as Tallent writes them. */
    for (int c = 0; c < CHANNELS; c++) {
        const struct grain_value *g = &p->ch[c].grain;
        int n[TA_NUMBERS_MAX], differs = 0;

        for (int f = 0; f < TA_GRAIN_FIELDS; f++) {
            n[f] = g->value[f];
            differs |= n[f] != grain_ranges[f].initial;
        }
        if (differs) {
            ta_put(w, "Grain", c, -1, n, TA_GRAIN_FIELDS, 0);
        }
        if (ta_routes_out(g->source, g->amount, n, TA_GRAIN_ROUTES)) {
            ta_put(w, "GrainCV", c, -1, n, 2 * TA_GRAIN_ROUTES, 0);
        }
    }
    /* Then Time Lock, as Tallent writes it: the channel's line when on or
     * its tempo is set (all five numbers then), each zone's Beats when set. */
    for (int c = 0; c < CHANNELS; c++) {
        const struct timelock_value *t = &p->ch[c].tl;
        int n[5] = { t->on, t->tempo, t->speed, t->input, t->ppqn };
        int tempo = t->tempo || t->speed || t->input != TA_SRC_OFF || t->ppqn;

        if (t->on || tempo) {
            ta_put(w, "TimeLock", c, -1, n, tempo ? 5 : 1, 0);
        }
        for (int z = 0; z < ZONES; z++) {
            if ((n[0] = p->ch[c].s.zone[z].beats)) {
                ta_put(w, "TimeLock", c, z, n, 1, 0);
            }
        }
    }
    int mix = 0;

    for (int i = 0; i < TA_MIXFX_BYTES; i++) {
        mix |= p->mixfx[i];
    }
    if (!ta_written && mix) {
        ta_written = 1;
        put_kv(w, 2, "ExtrasFormat", TA_EXTRAS_FORMAT, 0, 0, 0);
    }
    if (!ta_written && k) {
        for (uint32_t i = 0; i + 2 <= k->used; i += 2u + k->pool[i + 1]) {
            if (k->pool[i] == SEC_EXTRAS) {
                put_kv(w, 2, "ExtrasFormat", TA_EXTRAS_FORMAT, 0, 0, 0);
                break;
            }
        }
    }
    put_kept(w, k, SEC_EXTRAS, 2);
    /* Last, as Tallent writes them: a line for each 16 bytes not all 0. */
    for (int c = 0; mix && c < TA_MIXFX_BYTES / TA_MIXFX_LINE; c++) {
        const uint8_t *b = p->mixfx + c * TA_MIXFX_LINE;
        int any = 0;

        for (int i = 0; i < TA_MIXFX_LINE; i++) {
            any |= b[i];
        }
        if (!any) {
            continue;
        }
        indent(w, 2);
        puts_(w, "MixFX");
        put_fixed(w, c + 1, 0, 0, 0);
        puts_(w, " :");
        for (int i = 0; i < TA_MIXFX_LINE; i++) {
            putc_(w, ' ');
            put_fixed(w, b[i], 0, 0, 0);
        }
        putc_(w, '\n');
    }
}

int yml_write(const struct preset *p, const struct yml_keep *k, int number, char *out, size_t cap)
{
    struct wr w = { out, cap, 0 };

    if (!p || !out) {
        return YML_E_ARG;
    }
    put_number(&w, "Preset", number);
    if (p->name[0]) {
        char name[PRESET_NAME_LEN + 1];

        memcpy(name, p->name, sizeof(name));
        name[PRESET_NAME_LEN] = 0;
        puts_(&w, "  Name : ");
        puts_(&w, name);
        putc_(&w, '\n');
    }
    if (p->d2cv) {
        puts_(&w, "  Data2asCV : ");
        put_src(&w, CV_ABS + p->d2cv - 1);
        putc_(&w, '\n');
    }
    for (int g = 0; g < XFADE_GROUPS; g++) {
        char key[] = "XfadeACV", wk[] = "XfadeAWidth";

        key[5] = wk[5] = (char)('A' + g);
        if (p->xfade_cv[g]) {
            put_key(&w, 2, key);
            put_src(&w, CV_ABS + p->xfade_cv[g] - 1);
            putc_(&w, '\n');
        }
        if (p->xfade_width[g] != XFW_DEFAULT) {
            put_kv(&w, 2, wk, p->xfade_width[g], 2, 0, 0);
        }
    }
    if (p->reverb_unit) {
        put_kv(&w, 2, "ReverbUnit", p->reverb_unit, 0, 0, 0);
        put_kv(&w, 2, "ReverbProgram", p->reverb_program, 0, 0, 0);
    }
    for (unsigned e = 0; e < reverb_edits(p->reverb_unit) && e < REVERB_EDITS; e++) {
        char key[32] = "Reverb";
        const struct reverb_route *r = &p->reverb_route[e];
        const char *name = reverb_edit_key(p->reverb_unit, e);

        memcpy(key + 6, name, slen(name) + 1);
        if (p->reverb_edit[e] >= 0) {
            put_kv(&w, 2, key, p->reverb_edit[e], 0, 0, 0);
        }
        if (r->source >= 0 && r->source < CV_SRC_COUNT - CV_ABS) {
            memcpy(key + slen(key), "Mod", 4);
            put_key(&w, 2, key);
            put_src(&w, CV_ABS + r->source);
            putc_(&w, ' ');
            put_fixed(&w, r->amount, 2, 0, 4);
            putc_(&w, '\n');
        }
    }
    put_kept(&w, k, SEC_PRESET, 2);
    for (int c = 0; c < CHANNELS; c++) {
        put_channel(&w, p, k, c);
    }
    ta_put_all(&w, p, k);
    return w.len > cap ? YML_E_SPACE : (int)w.len;
}
