/*
 * Preset-building helpers shared by the card generators (presets.c and
 * friends). Presets are written by the firmware's own yml_write, so a
 * generated file is exactly what the firmware would save.
 */
#ifndef PRESETKIT_H
#define PRESETKIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "preset.h"
#include "voices.h"
#include "yml.h"

enum {
    TRX_BD = 1, TRX_SD, TRX_XT, TRX_CP, TRX_RS, TRX_CB, TRX_CH, TRX_OH, TRX_CY, TRX_MA,
    TRX_CL, TRX_XC, TRX_B2, EFM_BD, EFM_SD, EFM_XT, EFM_CP, EFM_RS, EFM_CB, EFM_HH,
    EFM_CY, PI_BD, PI_SD, PI_MT, PI_ML, PI_MA, PI_RS, PI_RC, PI_CC, PI_HH,
    GND_SN, GND_NS, GND_IM, SW_SAW, SID_6581, SW_PULSE, SW_ENS, FM_STAT, FM_PAR,
    FM_DYN, VO_6, GND_SIN, GND_NOIS
};

/* An absolute CV source: jack(1, 'C') is input 1C. */
#define jack(ch, letter) (CV_ABS + ((ch) - 1) * 3 + ((letter) - 'A'))

static struct preset p;
static const char *outdir;


static void kit(const char *name)
{
    memset(&p, 0, sizeof(p));
    p.magic = PRESET_MAGIC;
    p.version = PRESET_VERSION;
    strncpy(p.name, name, PRESET_NAME_LEN);
    for (int c = 0; c < CHANNELS; c++) {
        struct channel_cfg *ch = &p.ch[c];
        ch->mix_on = 1;
        ch->mode = MODE_MASTER;
        ch->mix = -60;                  /* -6 dB into the mix */
        ch->bits = BITS_MAX;
        for (int d = 0; d < D_COUNT; d++) {
            ch->mod[d].src = CV_OFF;
            ch->mod[d].amt = AMT_DEFAULT(d);
        }
        channel_fx_defaults(ch);
    }
}

/* Channel 1-8 runs machine m with its default controls. */
static void m(int c, int machine)
{
    const struct voice_desc *v = voices[machine];
    struct channel_cfg *ch = &p.ch[c - 1];

    ch->machine = (uint8_t)machine;
    ch->s.play_mode = PLAY_DEFAULT(machine);
    for (unsigned i = 0; i < CONTROLS; i++) {
        ch->ctrl[i] = i < v->nparams ? (uint8_t)v->params[i].def : 64;
    }
}

static int control(int c, const char *name)
{
    const struct voice_desc *v = voices[p.ch[c - 1].machine];

    for (unsigned i = 0; i < v->nparams; i++) {
        if (!strcmp(v->params[i].name, name)) {
            return (int)i;
        }
    }
    fprintf(stderr, "%s: channel %d (%s) has no control %s\n", p.name, c, v->name, name);
    exit(1);
}

/* Set a machine control by name, 0..127. */
static void k(int c, const char *name, int value)
{
    p.ch[c - 1].ctrl[control(c, name)] = (uint8_t)value;
}

/* Route a CV to a destination (CV_REL_A.. or CV_ABS + n), amount in 1/100. */
static void cv(int c, int dest, int src, int amt)
{
    p.ch[c - 1].mod[dest].src = (uint8_t)src;
    p.ch[c - 1].mod[dest].amt = (int16_t)amt;
}

static void save(int slot)
{
    static char out[65536];
    char path[512];
    FILE *f;
    int n = yml_write(&p, NULL, slot, out, sizeof(out));

    snprintf(path, sizeof(path), "%s/prst%03d.yml", outdir, slot);
    f = fopen(path, "wb");
    if (n < 0 || !f || fwrite(out, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        exit(1);
    }
    fclose(f);
    printf("%s  %s\n", path, p.name);
}

#endif
