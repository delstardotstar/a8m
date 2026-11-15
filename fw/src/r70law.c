/* The 70's parameter laws (sections 6, 7 and 10.4 of its description):
 * stored values, patches and edits to effective values, control values
 * and network values. Runs in the foreground. */
#include "r70.h"

#include <string.h>

#define SRC_SOFT_KNOB   69

static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

const struct r70_program *r70_find(unsigned number)
{
    return number >= 1 && number <= r70_program_count ? &r70_programs[number - 1] : 0;
}

static int ctl_b(const struct r70_param *q, int v)
{
    int i = v - q->lo;
    return q->b_tab >= 0 ? r70_tables[q->b_tab + i] : q->b_a * i + q->b_c;
}

static int ctl_count(const struct r70_param *q, int v)
{
    int i = v - q->lo;
    return q->c_tab >= 0 ? r70_tables[q->c_tab + i] : q->c_a * i + q->c_c;
}

static int attack(int hall, int row, int b)
{
    return r70_attack[hall][row][b & 255];
}

static int law(int id, int arg, int b, int count, int hall)
{
    switch (id) {
    case R70_L_X8: return b >> 3;
    case R70_L_NX8: return -(b >> 3);
    case R70_L_Y32: return imin(31, 32 - (b >> 3));
    case R70_L_YLC: return 32 - (b >> 3);
    case R70_L_Y8: return imin(7, (32 - (b >> 3)) >> 2);
    case R70_L_X32: return b >> 5;
    case R70_L_NX32: return -(b >> 5);
    case R70_L_FB: return b >= 128 ? (b - 128) >> 4 : -imin(7, (131 - b) >> 4);
    case R70_L_RES: return imax(-31, imin(31, (b - 128) >> 2));
    case R70_L_DEF: return imin(8, (5 * (63 - (b >> 2))) >> 3);
    case R70_L_HCRTY: return b < 8 ? 32 : 64 - (b >> 3);
    case R70_L_G2: return (5 * b) >> 6;
    case R70_L_G4: return b >> 2;
    case R70_L_OTAP: return b > 128 ? 5 : 3;
    case R70_L_INP: return b >= 253 ? 0 : 20;
    case R70_L_GINF: return b >= 253 ? 32 : b >> 3;
    case R70_L_PDL: return 128 * b + 1;
    case R70_L_DIF: return imin(26, (arg * (b >> 2)) >> 3);
    case R70_L_NDIF: return -imin(26, (arg * (b >> 2)) >> 3);
    case R70_L_ATT32: return attack(hall, arg, b);
    case R70_L_NATT32: return -attack(hall, arg, b);
    case R70_L_ATT8: return imin(attack(hall, arg, b), 31) >> 2;
    case R70_L_DLYH: return ((count >> 8) > arg ? 257 * arg : count) + 1;
    }
    return 0;
}

static int shelf(int stages, int l, int m)
{
    int a = (l >> 3) - (m >> 3), mag = a < 0 ? -a : a, v;

    if (stages >= 2) {
        int c = (m >> 3) << 3, x = c;
        for (int i = 0; i < stages - 2; i++) {
            x = (x * c) >> 8;
        }
        x = imax(x, 64);
        v = imin(((mag & 63) << 8) / x, 31) >> 2;
    } else {
        v = imin(mag, 31);
    }
    return a < 0 ? -v : v;
}

static const struct r70_param *role_param(const struct r70_algo *a, int role)
{
    for (unsigned i = 0; i < a->nparams; i++) {
        if (a->params[i].role == role) {
            return &a->params[i];
        }
    }
    return 0;
}

void r70_rt(const struct r70_algo *a, int l, int m, int d, int n, int16_t *coef)
{
    const struct r70_netinfo *ni = &r70_netinfo[a->net];
    const struct r70_param *q = role_param(a, R70_R_RT_MID);

    if (!ni->stages) {
        return;
    }
    if (q) {
        for (unsigned j = 0; j < 2; j++) {
            if (a->rtsym[j] == 255) {
                continue;
            }
            for (unsigned t = 0; t < q->ntgt; t++) {
                const struct r70_target *g = &r70_targets[q->tgt + t];
                if (!g->kind && g->idx == a->rtsym[j]) {
                    coef[g->idx] = (int16_t)law(g->law, g->arg, m, 0, a->net == R70_NET_HALL);
                }
            }
        }
    }
    coef[ni->shelf] = (int16_t)shelf(ni->stages, l, m);
    int L = (63 - (d >> 2)) >> 1, x = imin(m >> 3, L);
    for (unsigned j = 0; j < ni->ninloop; j++) {
        coef[ni->inloop[j]] = (int16_t)imax(0, imin(16, (ni->slope[j] * x) >> 2) - n);
    }
}

/* Stored values with the edits; a MASTER edit moves its row's voices by
 * as much (6.4). */
static void edited(const struct r70_program *p, const int16_t *edit, int16_t *v)
{
    const struct r70_algo *a = &r70_algos[p->jalgo];

    memcpy(v, p->value, sizeof(p->value));
    for (unsigned f = 0; edit && f < a->nfields; f++) {
        int e = edit[a->field_edit[f]], pi = a->field_param[f];
        const struct r70_param *q = &a->params[pi];

        if (e < 0) {
            continue;
        }
        int nv = q->lo + e, delta = nv - v[pi];
        v[pi] = (int16_t)nv;
        if (q->flags & R70_F_MASTER) {
            for (unsigned j = 0; j < a->nparams; j++) {
                if (a->params[j].pos / 10 == q->pos / 10 && a->params[j].pos % 10) {
                    v[j] = (int16_t)(v[j] + delta);
                }
            }
        }
    }
}

void r70_build(const struct r70_program *p, const int16_t *edit, struct r70_values *o)
{
    const struct r70_algo *a = &r70_algos[p->jalgo];
    const struct r70_netinfo *ni = &r70_netinfo[a->net];
    int16_t stored[R70_PARAMS];
    int32_t acc[R70_PARAMS];
    int count[R70_PARAMS];
    int hall = a->net == R70_NET_HALL;

    memset(o, 0, sizeof(*o));
    o->jalgo = p->jalgo;
    o->net = a->net;
    edited(p, edit, stored);
    memset(acc, 0, sizeof(acc));
    int sk = 0;
    for (unsigned i = 0; i < a->nparams; i++) {
        if (a->params[i].pos == 2) {
            sk = stored[i] - a->params[i].lo;
        }
    }
    /* 6.2: only the SOFT KNOB source has a value here. */
    for (unsigned j = 0; j < p->npatches; j++) {
        int s = p->patch[j].scale, sp = s >= 0 ? s + 1 : s, row = p->patch[j].dest / 10;
        if (p->patch[j].source != SRC_SOFT_KNOB || !sk) {
            continue;
        }
        for (unsigned i = 0; i < a->nparams; i++) {
            const struct r70_param *q = &a->params[i];
            if (q->pos == p->patch[j].dest) {
                acc[i] += sp * sk;
                if (q->flags & R70_F_MASTER) {
                    for (unsigned k = 0; k < a->nparams; k++) {
                        if (a->params[k].pos / 10 == row && a->params[k].pos % 10) {
                            acc[k] += sp * sk;
                        }
                    }
                }
            }
        }
    }
    for (unsigned i = 0; i < a->nparams; i++) {
        const struct r70_param *q = &a->params[i];
        int v = stored[i] + floordiv(64 + acc[i], 128);
        o->eff[i] = (int16_t)imax(q->lo, imin(q->hi, v));
    }
    for (unsigned j = 0; j < a->nbase; j++) {
        if (a->base[j].kind) {
            o->delay[a->base[j].idx] = (uint16_t)a->base[j].value;
        } else {
            o->coef[a->base[j].idx] = a->base[j].value;
        }
    }
    if (a->has_rtlow) o->ctl[R70_C_RT_LOW] = a->rtlow;
    if (a->has_chorus) o->ctl[R70_C_CHORUS] = a->chorus;
    if (a->has_dopt) o->ctl[R70_C_DECAY_OPT] = a->dopt;
    for (unsigned i = 0; i < a->nparams; i++) {
        const struct r70_param *q = &a->params[i];
        int v = o->eff[i], b = q->flags & R70_F_B ? ctl_b(q, v) : 0, c = q->flags & R70_F_COUNT ? ctl_count(q, v) : 0;

        count[i] = c;
        for (unsigned k = 0; k < q->nctl; k++) {
            o->ctl[q->ctl[k].field] = (int16_t)(q->ctl[k].tab >= 0 ? r70_tables[q->ctl[k].tab + v - q->lo] : b);
        }
        for (unsigned t = 0; t < q->ntgt; t++) {
            const struct r70_target *g = &r70_targets[q->tgt + t];
            int x = law(g->law, g->arg, b, c, hall);
            if (g->kind) {
                o->delay[g->idx] = (uint16_t)x;
            } else {
                o->coef[g->idx] = (int16_t)x;
            }
        }
        if (q->pos == 1) {
            o->fxgain = r70_fxadj[v + 51];
        }
    }
    for (unsigned j = 0; j < ni->nmods; j++) {
        o->coef[ni->mods[j].k] = 0;
    }
    const struct r70_param *rate = role_param(a, R70_R_RATE);
    if (rate) {
        int ti = imin(127, imax(0, o->eff[rate - a->params] - rate->lo)), tb = r70_tempo[ti] / 24;
        for (unsigned i = 0; i < a->nparams; i++) {
            const struct r70_param *q = &a->params[i];
            int n = o->eff[i] - q->lo, cnt;
            if (!q->r_unit) {
                continue;
            }
            if (q->r_max_beats && tb) {
                n = imin(n, q->r_max_beats / tb);
            }
            if (q->r_unit == 128) {
                cnt = 128 * imin(196, 2 * ((n * tb + 128) / 256));
            } else {
                cnt = n * tb;
                if (q->r_max_high && (cnt >> 8) > q->r_max_high) {
                    cnt = 257 * q->r_max_high;
                }
            }
            for (unsigned k = 0; k < 6 && q->r_taps[k] != 255; k++) {
                o->delay[q->r_taps[k]] = (uint16_t)(cnt + q->r_offset);
            }
        }
    }
    if (ni->size) {
        int s = imin(o->ctl[R70_C_SIZE], ni->smax);
        for (unsigned j = 0; j < ni->nsized; j++) {
            o->delay[ni->sizetd[j]] = ni->size[s * ni->nsized + j];
        }
        for (unsigned j = 0; j < ni->nmods; j++) {
            o->delay[ni->mods[j].A] = (uint16_t)(ni->moff[j] + ni->mods[j].a0);
            o->delay[ni->mods[j].B] = (uint16_t)(ni->moff[j] + ni->mods[j].b0);
        }
        for (unsigned i = 0; i < a->nparams; i++) {
            const struct r70_param *q = &a->params[i];
            for (unsigned k = 0; k < q->ntaps; k++) {
                unsigned d = q->taps[k], off = 0;
                for (unsigned m = 0; m < ni->nsizeoff; m++) {
                    if (ni->sizeoff[m] == d) {
                        off = ni->sizeoff[ni->nsizeoff + m];
                    }
                }
                o->delay[d] = (uint16_t)(imin(count[i], ni->sizemax[s]) + (int)off);
            }
        }
        r70_rt(a, o->ctl[R70_C_RT_LOW], o->ctl[R70_C_RT_MID], o->ctl[R70_C_DEFINITION], 0, o->coef);
    } else {
        for (unsigned j = 0; j < ni->nmods; j++) {
            o->delay[ni->mods[j].A] = (uint16_t)(ni->mods[j].offset + ni->mods[j].a0);
            o->delay[ni->mods[j].B] = (uint16_t)(ni->mods[j].offset + ni->mods[j].b0);
        }
    }
}

const char *r70_text(const struct r70_param *q, int v)
{
    return q->text >= 0 ? r70_texts[q->text + v - q->lo] : "";
}

/* ---- the page's fields ---- */

static const struct r70_param *field_param(const struct r70_program *p, unsigned e)
{
    const struct r70_algo *a = &r70_algos[p->jalgo];

    for (unsigned f = 0; f < a->nfields; f++) {
        if (a->field_edit[f] == e) {
            return &a->params[a->field_param[f]];
        }
    }
    return 0;
}

unsigned r70_fields(const struct r70_program *p, uint8_t edit[R70_FIELDS_MAX])
{
    const struct r70_algo *a = &r70_algos[p->jalgo];
    unsigned n = 0, soft = 0;

    for (unsigned j = 0; j < p->npatches; j++) {
        soft |= p->patch[j].source == SRC_SOFT_KNOB;
    }
    for (unsigned f = 0; f < a->nfields; f++) {
        if (a->field_edit[f] != R70_E_SOFTKNOB || soft) {
            edit[n++] = a->field_edit[f];
        }
    }
    return n;
}

static const char *const suffixes[] = { " MS", " BPM", " %", " S", " M" };

const char *r70_edit_name(const struct r70_program *p, unsigned e, char *out)
{
    const struct r70_param *q = field_param(p, e);
    unsigned n;

    out[0] = 0;
    if (!q) {
        return out;
    }
    n = (unsigned)strlen(q->name);
    for (unsigned j = 0; j < sizeof(suffixes) / sizeof(suffixes[0]); j++) {
        unsigned k = (unsigned)strlen(suffixes[j]);
        if (n > k && !memcmp(q->name + n - k, suffixes[j], k)) {
            n -= k;
            break;
        }
    }
    n = n > 15 ? 15 : n;
    memcpy(out, q->name, n);
    out[n] = 0;
    return out;
}

const char *r70_edit_key(unsigned e)
{
    return e < R70_EDITS ? r70_edit_keys[e] : "";
}

void r70_edit_range(const struct r70_program *p, unsigned e, int *lo, int *hi)
{
    const struct r70_param *q = field_param(p, e);

    *lo = 0;
    *hi = q ? q->hi - q->lo : 0;
}

int r70_edit_base(const struct r70_program *p, unsigned e)
{
    const struct r70_algo *a = &r70_algos[p->jalgo];
    const struct r70_param *q = field_param(p, e);

    return q ? p->value[q - a->params] - q->lo : 0;
}

/* The text, with the unit the name carried when the text ends in a digit. */
void r70_edit_format(const struct r70_program *p, unsigned e, int v, char *out)
{
    const struct r70_param *q = field_param(p, e);
    const char *t;
    unsigned n, nl;

    out[0] = 0;
    if (!q) {
        return;
    }
    t = r70_text(q, q->lo + v);
    n = (unsigned)strlen(t);
    n = n > 11 ? 11 : n;
    memcpy(out, t, n);
    out[n] = 0;
    nl = (unsigned)strlen(q->name);
    if (n && t[n - 1] >= '0' && t[n - 1] <= '9') {
        for (unsigned j = 0; j < sizeof(suffixes) / sizeof(suffixes[0]); j++) {
            unsigned k = (unsigned)strlen(suffixes[j]);
            if (nl > k && !memcmp(q->name + nl - k, suffixes[j], k)) {
                if (suffixes[j][1] == '%') {
                    memcpy(out + n, "%", 2);
                } else if (suffixes[j][1] != 'S') {
                    memcpy(out + n, suffixes[j], k + 1);
                }
                break;
            }
        }
    }
}
