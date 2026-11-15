#include "preset.h"
#include "voices.h"
#include "eq.h"

#include <string.h>

struct preset preset;
int preset_number = 1;

/* Machines in the default kit, channel 1-8 (voices[] indices). */

void preset_set_machine(struct channel_cfg *c, int machine)
{
    const struct voice_desc *v = voices[machine];

    if (PLAY_DEFAULT(c->machine) != PLAY_DEFAULT(machine)) {
        c->s.play_mode = PLAY_DEFAULT(machine);
    }
    c->machine = (uint8_t)machine;
    for (unsigned i = 0; i < CONTROLS; i++) {
        c->ctrl[i] = v && i < v->nparams ? (uint8_t)v->params[i].def : 64;
        c->mod[D_CTRL + i].src = CV_OFF;
        c->mod[D_CTRL + i].amt = 100;
    }
}

void preset_init(struct preset *p)
{
    memset(p, 0, sizeof(*p));
    p->magic = PRESET_MAGIC;
    p->version = PRESET_VERSION;
    memcpy(p->name, "INIT", 5);
    for (int g = 0; g < XFADE_GROUPS; g++) {
        p->xfade_width[g] = XFW_DEFAULT;
    }
    for (int e = 0; e < REVERB_EDITS; e++) {
        p->reverb_edit[e] = -1;
        p->reverb_route[e].source = TA_SRC_OFF;
    }
    for (int c = 0; c < CHANNELS; c++) {
        struct channel_cfg *ch = &p->ch[c];

        ch->mix_on = 1;
        ch->level = 0;
        ch->mix = 0;
        ch->bits = BITS_MAX;
        for (int d = 0; d < D_COUNT; d++) {
            ch->mod[d].src = CV_OFF;
            ch->mod[d].amt = AMT_DEFAULT(d);
        }
        sampler_defaults(&ch->s);
        channel_fx_defaults(ch);
        preset_set_machine(ch, 0);
    }
}

int preset_param_channel(int ch)
{
    while (ch > 0 && preset.ch[ch].mode == MODE_STEREO) {
        ch--;
    }
    return ch;
}

int preset_has_offsets(const struct channel_cfg *c)
{
    for (int i = 0; i < PO_COUNT; i++) {
        if (c->ofs[i]) {
            return 1;
        }
    }
    return 0;
}

void preset_offset_range(int k, int machine, int *lo, int *hi)
{
    switch (k) {
    case PO_PITCH:   *lo = machine ? -2400 : SPITCH_MIN; *hi = machine ? 2400 : SPITCH_MAX; return;
    case PO_LEVEL:   *lo = machine ? MIX_MIN : SLEVEL_MIN; *hi = machine ? LEVEL_MAX : SLEVEL_MAX; return;
    case PO_MIX:     *lo = MIX_MIN; *hi = LEVEL_MAX; return;
    case PO_BITS:    *lo = BITS_MIN; *hi = BITS_MAX; return;
    case PO_ATTACK:
    case PO_RELEASE: *lo = 0; *hi = ENV_STEPS; return;
    case PO_ALIAS:
    case PO_PMINDEX: *lo = 0; *hi = 100; return;
    case PO_DELAY:   *lo = 0; *hi = SDELAY_MAX; return;
    case PO_EQGAIN:
    case PO_EQGAIN + 1:
    case PO_EQGAIN + 2: *lo = -EQ_GAIN_MAX; *hi = EQ_GAIN_MAX; return;
    default:         *lo = 0; *hi = 127; return;
    }
}

int preset_offset_apply(int k, int machine, int v, int o)
{
    int lo, hi;

    if (!o) {
        return v;
    }
    preset_offset_range(k, machine, &lo, &hi);
    v += o;
    return v < lo ? lo : v > hi ? hi : v;
}

int preset_drop_sample(struct preset *p, unsigned smp)
{
    int n = 0;

    for (int c = 0; c < CHANNELS; c++) {
        struct sampler_cfg *s = &p->ch[c].s;

        for (int z = 0; z < ZONES;) {
            if (s->zone[z].smp != smp) {
                z++;
                continue;
            }
            for (int k = z; k + 1 < ZONES; k++) {
                s->zone[k] = s->zone[k + 1];
            }
            zone_defaults(&s->zone[ZONES - 1]);
            n++;
        }
    }
    return n;
}
