/* Voice descriptors, in machine-id order. */
#include "voices.h"

extern const mdkit_voice mdkit_voice_trx_bd;
extern const mdkit_voice mdkit_voice_trx_sd;
extern const mdkit_voice mdkit_voice_trx_xt;
extern const mdkit_voice mdkit_voice_trx_cp;
extern const mdkit_voice mdkit_voice_trx_rs;
extern const mdkit_voice mdkit_voice_trx_cb;
extern const mdkit_voice mdkit_voice_trx_ch;
extern const mdkit_voice mdkit_voice_trx_oh;
extern const mdkit_voice mdkit_voice_trx_cy;
extern const mdkit_voice mdkit_voice_trx_ma;
extern const mdkit_voice mdkit_voice_trx_cl;
extern const mdkit_voice mdkit_voice_trx_xc;
extern const mdkit_voice mdkit_voice_trx_b2;
extern const mdkit_voice mdkit_voice_efm_bd;
extern const mdkit_voice mdkit_voice_efm_sd;
extern const mdkit_voice mdkit_voice_efm_xt;
extern const mdkit_voice mdkit_voice_efm_cp;
extern const mdkit_voice mdkit_voice_efm_rs;
extern const mdkit_voice mdkit_voice_efm_cb;
extern const mdkit_voice mdkit_voice_efm_hh;
extern const mdkit_voice mdkit_voice_efm_cy;
extern const mdkit_voice mdkit_voice_pi_bd;
extern const mdkit_voice mdkit_voice_pi_sd;
extern const mdkit_voice mdkit_voice_pi_mt;
extern const mdkit_voice mdkit_voice_pi_ml;
extern const mdkit_voice mdkit_voice_pi_ma;
extern const mdkit_voice mdkit_voice_pi_rs;
extern const mdkit_voice mdkit_voice_pi_rc;
extern const mdkit_voice mdkit_voice_pi_cc;
extern const mdkit_voice mdkit_voice_pi_hh;
extern const mdkit_voice mnmd_voice_gnd_sn;
extern const mdkit_voice mnmd_voice_gnd_ns;
extern const mdkit_voice mnmd_voice_gnd_im;
extern const struct pitched_voice mnmd_voice_swave_saw;
extern const struct pitched_voice mnmd_voice_sid_6581;
extern const struct pitched_voice mnmd_voice_swave_pulse;
extern const struct pitched_voice mnmd_voice_swave_ens;
extern const struct pitched_voice mnmd_voice_fmpstat;
extern const struct pitched_voice mnmd_voice_fmppar;
extern const struct pitched_voice mnmd_voice_fmpdyn;
extern const struct pitched_voice mnmd_voice_vo_6;
extern const struct pitched_voice mnmd_voice_gnd_sin;
extern const struct pitched_voice mnmd_voice_gnd_nois;
int mnmd_swave_ens_step(unsigned v);

#define P(n, d, t) { n, 0, 127, d, PF_INT, TAG_##t, 0 }
#define C(n, d, t, ch) { n, 0, 127, d, PF_CHOICE, TAG_##t, &ch }

int param_choice_index(const struct param_choice *c, int v)
{
    uint32_t q = v >= 127 ? 0x7fffffffu : (uint32_t)v * 16909320u;
    uint32_t i = c->step ? (uint32_t)c->step(q >> 24) : c->q15 ? ((q >> 16) * c->n) >> 15 : ((q >> 24) * c->n) >> 7;

    return i < c->n ? (int)i : c->n - 1;
}

int param_choice_value(const struct param_choice *c, int i)
{
    int lo = -1, hi = 0;

    for (int v = 0; v < 128; v++) {
        if (param_choice_index(c, v) == i) {
            if (lo < 0) {
                lo = v;
            }
            hi = v;
        }
    }
    return lo < 0 ? 0 : (lo + hi) / 2;
}

static const char *const l_offon[] = { "OFF", "ON" };
static const struct param_choice ch_offon = { 2, 0, l_offon, 0 };
static const char *const l_wave[] = { "TRI", "SAW", "PULS", "MIX", "NOIS" };
static const struct param_choice ch_wave = { 5, 0, l_wave, 0 };
static const char *const l_sidmod[] = { "OFF", "RING", "SYNC", "R+S" };
static const struct param_choice ch_sidmod = { 4, 0, l_sidmod, 0 };
static const char *const l_msrc[] = { "MFRQ", "PRCH" };
static const struct param_choice ch_msrc = { 2, 0, l_msrc, 0 };
static const char *const l_ratio[] = {
    "1:64", "1:32", "1:16", "3:32", "1:8", "5:32", "3:16", "1:4", "5:16", "3:8", "7:16", "1:2",
    "5:8", "3:4", "7:8", "1", "1.25", "1.5", "1.75", "2", "2.5", "3", "3.5", "4",
};
static const struct param_choice ch_ratio = { 24, 1, l_ratio, 0 };
static const char *const l_cons[] = {
    "-", "B", "D", "F", "G", "H", "J", "K", "L", "M", "N", "P", "R", "RR", "S", "SJ", "T", "TJ", "TH", "V", "Z",
};
static const struct param_choice ch_cons = { 21, 1, l_cons, 0 };
#define N(p) (uint8_t)(sizeof(p) / sizeof((p)[0]))

static const struct voice_param p_trx_bd[] = {
    P("PTCH", 64, PITCH), P("DEC", 64, ENV), P("RAMP", 0, PITCH), P("RDEC", 0, PITCH),
    P("STRT", 0, TIMBRE), P("NOIS", 0, TIMBRE), P("HARM", 0, TIMBRE), P("CLIP", 0, TIMBRE),
};
static const struct voice_desc v_trx_bd = {
    "TRX-BD", FAM_TRX, N(p_trx_bd), 0, 18, p_trx_bd, &mdkit_voice_trx_bd, 1.0002f,
};
static const struct voice_param p_trx_sd[] = {
    P("PTCH", 35, PITCH), P("DEC", 7, ENV), P("BUMP", 0, PITCH), P("BENV", 64, PITCH),
    P("SNAP", 127, TIMBRE), P("TONE", 0, TIMBRE), P("TUNE", 100, TIMBRE), P("CLIP", 90, TIMBRE),
};
static const struct voice_desc v_trx_sd = {
    "TRX-SD", FAM_TRX, N(p_trx_sd), 0, 19, p_trx_sd, &mdkit_voice_trx_sd, 1.0000f,
};
static const struct voice_param p_trx_xt[] = {
    P("PTCH", 70, PITCH), P("DEC", 25, ENV), P("RAMP", 50, PITCH), P("RDEC", 30, PITCH),
    P("DAMP", 64, TIMBRE), P("DIST", 32, TIMBRE), P("DTYP", 0, TIMBRE),
};
static const struct voice_desc v_trx_xt = {
    "TRX-XT", FAM_TRX, N(p_trx_xt), 0, 9, p_trx_xt, &mdkit_voice_trx_xt, 1.3855f,
};
static const struct voice_param p_trx_cp[] = {
    P("CLPY", 50, TIMBRE), P("TONE", 100, TIMBRE), P("HARD", 7, TIMBRE), P("RICH", 127, TIMBRE),
    P("RATE", 40, TIMBRE), P("ROOM", 64, TIMBRE), P("RSIZ", 0, TIMBRE), P("RTUN", 90, TIMBRE),
};
static const struct voice_desc v_trx_cp = {
    "TRX-CP", FAM_TRX, N(p_trx_cp), 0, 30, p_trx_cp, &mdkit_voice_trx_cp, 3.3141f,
};
static const struct voice_param p_trx_rs[] = {
    P("PTCH", 50, PITCH), P("DEC", 0, ENV), P("DIST", 50, TIMBRE),
};
static const struct voice_desc v_trx_rs = {
    "TRX-RS", FAM_TRX, N(p_trx_rs), 0, 15, p_trx_rs, &mdkit_voice_trx_rs, 3.2321f,
};
static const struct voice_param p_trx_cb[] = {
    P("PTCH", 100, PITCH), P("DEC", 50, ENV), P("ENH", 127, TIMBRE), P("DAMP", 127, TIMBRE),
    P("TONE", 0, TIMBRE), P("BUMP", 0, PITCH),
};
static const struct voice_desc v_trx_cb = {
    "TRX-CB", FAM_TRX, N(p_trx_cb), 0, 18, p_trx_cb, &mdkit_voice_trx_cb, 2.8268f,
};
static const struct voice_param p_trx_ch[] = {
    P("GAP", 0, TIMBRE), P("DEC", 8, ENV), P("HPF", 0, TIMBRE), P("LPF", 127, TIMBRE),
    P("MTAL", 0, TIMBRE),
};
static const struct voice_desc v_trx_ch = {
    "TRX-CH", FAM_TRX, N(p_trx_ch), 0, 28, p_trx_ch, &mdkit_voice_trx_ch, 6.7329f,
};
static const struct voice_param p_trx_oh[] = {
    P("GAP", 50, TIMBRE), P("DEC", 60, ENV), P("HPF", 8, TIMBRE), P("LPF", 127, TIMBRE),
    P("MTAL", 0, TIMBRE),
};
static const struct voice_desc v_trx_oh = {
    "TRX-OH", FAM_TRX, N(p_trx_oh), 0, 31, p_trx_oh, &mdkit_voice_trx_oh, 5.8709f,
};
static const struct voice_param p_trx_cy[] = {
    P("RICH", 127, TIMBRE), P("DEC", 0, ENV), P("TOP", 0, TIMBRE), P("TTUN", 127, TIMBRE),
    P("SIZE", 0, TIMBRE), P("PEAK", 127, TIMBRE),
};
static const struct voice_desc v_trx_cy = {
    "TRX-CY", FAM_TRX, N(p_trx_cy), 0, 43, p_trx_cy, &mdkit_voice_trx_cy, 5.8468f,
};
static const struct voice_param p_trx_ma[] = {
    P("ATT", 32, ENV), P("SUS", 32, ENV), P("REV", 0, ENV), P("DAMP", 0, TIMBRE),
    P("RATL", 64, TIMBRE), P("RTYP", 64, TIMBRE), P("TONE", 64, TIMBRE), P("HARD", 64, TIMBRE),
};
static const struct voice_desc v_trx_ma = {
    "TRX-MA", FAM_TRX, N(p_trx_ma), 0, 22, p_trx_ma, &mdkit_voice_trx_ma, 8.4830f,
};
static const struct voice_param p_trx_cl[] = {
    P("PTCH", 64, PITCH), P("DEC", 32, ENV), P("DUAL", 64, TIMBRE), P("ENH", 0, TIMBRE),
    P("TUNE", 64, TIMBRE), P("CLIC", 0, TIMBRE),
};
static const struct voice_desc v_trx_cl = {
    "TRX-CL", FAM_TRX, N(p_trx_cl), 0, 16, p_trx_cl, &mdkit_voice_trx_cl, 4.5045f,
};
static const struct voice_param p_trx_xc[] = {
    P("PTCH", 64, PITCH), P("DEC", 32, ENV), P("RAMP", 32, PITCH), P("RDEC", 100, PITCH),
    P("DAMP", 0, TIMBRE), P("DIST", 0, TIMBRE), P("DTYP", 0, TIMBRE),
};
static const struct voice_desc v_trx_xc = {
    "TRX-XC", FAM_TRX, N(p_trx_xc), 0, 9, p_trx_xc, &mdkit_voice_trx_xc, 1.0457f,
};
static const struct voice_param p_trx_b2[] = {
    P("PTCH", 60, PITCH), P("DEC", 65, ENV), P("RAMP", 41, PITCH), P("HOLD", 62, ENV),
    P("TICK", 91, TIMBRE), P("NOIS", 44, TIMBRE), P("DIRT", 56, TIMBRE), P("DIST", 58, TIMBRE),
};
static const struct voice_desc v_trx_b2 = {
    "TRX-B2", FAM_TRX, N(p_trx_b2), 0, 15, p_trx_b2, &mdkit_voice_trx_b2, 0.9885f,
};
static const struct voice_param p_efm_bd[] = {
    P("PTCH", 0, PITCH), P("DEC", 64, ENV), P("RAMP", 32, PITCH), P("RDEC", 127, PITCH),
    P("MOD", 35, MOD), P("MFRQ", 14, MOD), P("MDEC", 64, MOD), P("MFB", 24, MOD),
};
static const struct voice_desc v_efm_bd = {
    "EFM-BD", FAM_EFM, N(p_efm_bd), 0, 35, p_efm_bd, &mdkit_voice_efm_bd, 1.6055f,
};
static const struct voice_param p_efm_sd[] = {
    P("PTCH", 27, PITCH), P("DEC", 60, ENV), P("NOIS", 14, TIMBRE), P("NDEC", 79, ENV),
    P("MOD", 0, MOD), P("MFRQ", 5, MOD), P("MDEC", 127, MOD), P("HPF", 29, TIMBRE),
};
static const struct voice_desc v_efm_sd = {
    "EFM-SD", FAM_EFM, N(p_efm_sd), 0, 23, p_efm_sd, &mdkit_voice_efm_sd, 5.7676f,
};
static const struct voice_param p_efm_xt[] = {
    P("PTCH", 25, PITCH), P("DEC", 50, ENV), P("RAMP", 127, PITCH), P("RDEC", 111, PITCH),
    P("MOD", 5, MOD), P("MFRQ", 4, MOD), P("MDEC", 79, MOD), P("CLIC", 0, TIMBRE),
};
static const struct voice_desc v_efm_xt = {
    "EFM-XT", FAM_EFM, N(p_efm_xt), 0, 15, p_efm_xt, &mdkit_voice_efm_xt, 1.9763f,
};
static const struct voice_param p_efm_cp[] = {
    P("PTCH", 50, PITCH), P("DEC", 40, ENV), P("CLPS", 127, TIMBRE), P("CDEC", 24, ENV),
    P("MOD", 115, MOD), P("MFRQ", 36, MOD), P("MDEC", 87, MOD), P("HPF", 8, TIMBRE),
};
static const struct voice_desc v_efm_cp = {
    "EFM-CP", FAM_EFM, N(p_efm_cp), 0, 22, p_efm_cp, &mdkit_voice_efm_cp, 1.4435f,
};
static const struct voice_param p_efm_rs[] = {
    P("PTCH", 67, PITCH), P("DEC", 10, ENV), P("MOD", 8, MOD), P("HPF", 15, TIMBRE),
    P("SNAR", 38, TIMBRE), P("SPTC", 25, TIMBRE), P("SDEC", 59, ENV), P("SMOD", 0, MOD),
};
static const struct voice_desc v_efm_rs = {
    "EFM-RS", FAM_EFM, N(p_efm_rs), 0, 24, p_efm_rs, &mdkit_voice_efm_rs, 1.2590f,
};
static const struct voice_param p_efm_cb[] = {
    P("PTCH", 54, PITCH), P("DEC", 61, ENV), P("SNAP", 92, TIMBRE), P("FB", 56, MOD),
    P("MOD", 17, MOD), P("MFRQ", 112, MOD), P("MDEC", 42, MOD),
};
static const struct voice_desc v_efm_cb = {
    "EFM-CB", FAM_EFM, N(p_efm_cb), 0, 25, p_efm_cb, &mdkit_voice_efm_cb, 1.4582f,
};
static const struct voice_param p_efm_hh[] = {
    P("PTCH", 26, PITCH), P("DEC", 54, ENV), P("TREM", 26, MOD), P("TFRQ", 47, MOD),
    P("MOD", 0, MOD), P("MFRQ", 86, MOD), P("MDEC", 93, MOD), P("FB", 75, MOD),
};
static const struct voice_desc v_efm_hh = {
    "EFM-HH", FAM_EFM, N(p_efm_hh), 0, 25, p_efm_hh, &mdkit_voice_efm_hh, 1.0105f,
};
static const struct voice_param p_efm_cy[] = {
    P("PTCH", 64, PITCH), P("DEC", 72, ENV), P("FB", 96, MOD), P("HPF", 92, TIMBRE),
    P("MOD", 45, MOD), P("MFRQ", 32, MOD), P("MDEC", 64, MOD),
};
static const struct voice_desc v_efm_cy = {
    "EFM-CY", FAM_EFM, N(p_efm_cy), 0, 31, p_efm_cy, &mdkit_voice_efm_cy, 1.8361f,
};
static const struct voice_param p_pi_bd[] = {
    P("PTCH", 64, PITCH), P("DEC", 35, ENV), P("HARD", 100, TIMBRE), P("HAMR", 0, TIMBRE),
    P("TENS", 127, TIMBRE), P("DAMP", 100, TIMBRE),
};
static const struct voice_desc v_pi_bd = {
    "PI-BD", FAM_PI, N(p_pi_bd), 0, 53, p_pi_bd, &mdkit_voice_pi_bd, 1.0000f,
};
static const struct voice_param p_pi_sd[] = {
    P("PTCH", 64, PITCH), P("DEC", 64, ENV), P("HARD", 10, TIMBRE), P("RING", 40, TIMBRE),
    P("TENS", 90, TIMBRE), P("RVOL", 80, TIMBRE), P("RDEC", 80, ENV),
};
static const struct voice_desc v_pi_sd = {
    "PI-SD", FAM_PI, N(p_pi_sd), 0, 61, p_pi_sd, &mdkit_voice_pi_sd, 1.0000f,
};
static const struct voice_param p_pi_mt[] = {
    P("PTCH", 20, PITCH), P("DEC", 20, ENV), P("HARD", 127, TIMBRE), P("HAMR", 127, TIMBRE),
    P("TUNE", 127, TIMBRE), P("DAMP", 80, TIMBRE), P("SIZE", 90, TIMBRE), P("POS", 64, TIMBRE),
};
static const struct voice_desc v_pi_mt = {
    "PI-MT", FAM_PI, N(p_pi_mt), 0, 49, p_pi_mt, &mdkit_voice_pi_mt, 1.0000f,
};
static const struct voice_param p_pi_ml[] = {
    P("PTCH", 0, PITCH), P("DEC", 64, ENV), P("HARD", 127, TIMBRE), P("TENS", 0, TIMBRE),
};
static const struct voice_desc v_pi_ml = {
    "PI-ML", FAM_PI, N(p_pi_ml), 0, 37, p_pi_ml, &mdkit_voice_pi_ml, 2.0664f,
};
static const struct voice_param p_pi_ma[] = {
    P("GRNS", 90, TIMBRE), P("DEC", 0, ENV), P("GLEN", 72, TIMBRE), P("SIZE", 96, TIMBRE),
    P("HARD", 64, TIMBRE),
};
static const struct voice_desc v_pi_ma = {
    "PI-MA", FAM_PI, N(p_pi_ma), 0, 14, p_pi_ma, &mdkit_voice_pi_ma, 1.9568f,
};
static const struct voice_param p_pi_rs[] = {
    P("PTCH", 90, PITCH), P("DEC", 48, ENV), P("HARD", 64, TIMBRE), P("RING", 64, TIMBRE),
    P("RVOL", 64, TIMBRE), P("RDEC", 64, ENV),
};
static const struct voice_desc v_pi_rs = {
    "PI-RS", FAM_PI, N(p_pi_rs), 0, 59, p_pi_rs, &mdkit_voice_pi_rs, 1.0000f,
};
static const struct voice_param p_pi_rc[] = {
    P("PTCH", 64, PITCH), P("DEC", 70, ENV), P("HARD", 96, TIMBRE), P("RING", 32, TIMBRE),
    P("AG", 127, TIMBRE), P("AU", 127, TIMBRE), P("BR", 64, TIMBRE), P("GRAB", 127, ENV),
};
static const struct voice_desc v_pi_rc = {
    "PI-RC", FAM_PI, N(p_pi_rc), 0, 50, p_pi_rc, &mdkit_voice_pi_rc, 2.2225f,
};
static const struct voice_param p_pi_cc[] = {
    P("PTCH", 64, PITCH), P("DEC", 70, ENV), P("HARD", 90, TIMBRE), P("RING", 32, TIMBRE),
    P("AG", 127, TIMBRE), P("AU", 127, TIMBRE), P("BR", 64, TIMBRE), P("GRAB", 127, ENV),
};
static const struct voice_desc v_pi_cc = {
    "PI-CC", FAM_PI, N(p_pi_cc), 0, 50, p_pi_cc, &mdkit_voice_pi_cc, 2.4425f,
};
static const struct voice_param p_pi_hh[] = {
    P("PTCH", 64, PITCH), P("DEC", 70, ENV), P("CLSN", 100, TIMBRE), P("RING", 30, TIMBRE),
    P("AG", 127, TIMBRE), P("AU", 127, TIMBRE), P("BR", 60, TIMBRE), P("CLOS", 127, ENV),
};
static const struct voice_desc v_pi_hh = {
    "PI-HH", FAM_PI, N(p_pi_hh), 0, 50, p_pi_hh, &mdkit_voice_pi_hh, 1.5837f,
};

static const struct voice_param p_gnd_sn[] = {
    P("PTCH", 64, PITCH), P("DEC", 50, ENV), P("RAMP", 40, PITCH), P("RDEC", 40, PITCH),
};
static const struct voice_desc v_gnd_sn = {
    "GND-SN", FAM_GND, N(p_gnd_sn), 0, 5, p_gnd_sn, &mnmd_voice_gnd_sn, 6.8790f,
};
static const struct voice_param p_gnd_ns[] = {
    P("DEC", 50, ENV),
};
static const struct voice_desc v_gnd_ns = {
    "GND-NS", FAM_GND, N(p_gnd_ns), 0, 5, p_gnd_ns, &mnmd_voice_gnd_ns, 2.1931f,
};
static const struct voice_param p_gnd_im[] = {
    P("UP", 64, ENV), P("UVAL", 127, LEVEL), P("DOWN", 64, ENV), P("DVAL", 0, LEVEL),
};
static const struct voice_desc v_gnd_im = {
    "GND-IM", FAM_GND, N(p_gnd_im), 0, 8, p_gnd_im, &mnmd_voice_gnd_im, 1.5505f,
};

#define MM (VF_PITCH_IN | VF_GATED)
static const struct voice_param p_swave_saw[] = {
    P("UNIL", 40, TIMBRE), P("UNIW", 30, TIMBRE), P("UNIX", 0, TIMBRE), P("SUBX", 0, TIMBRE),
    P("SUB1", 30, TIMBRE), P("SUB2", 0, TIMBRE), P("TUNE", 64, PITCH),
};
static const struct voice_desc v_swave_saw = {
    "SW-SAW", FAM_MNM, N(p_swave_saw), MM, 8, p_swave_saw, &mnmd_voice_swave_saw.v, 3.8290f,
};
static const struct voice_param p_sid_6581[] = {
    P("PW", 32, TIMBRE), P("PWAD", 0, MOD), C("PWRS", 32, MOD, ch_offon), C("WAVE", 38, TIMBRE, ch_wave),
    C("MOD", 16, MOD, ch_sidmod), C("MSRC", 32, MOD, ch_msrc), P("MFRQ", 64, MOD), P("TUNE", 64, PITCH),
};
static const struct voice_desc v_sid_6581 = {
    "SID-6581", FAM_MNM, N(p_sid_6581), MM, 9, p_sid_6581, &mnmd_voice_sid_6581.v, 1.7756f,
};

static const struct voice_param p_swave_pulse[] = {
    P("UNIL", 30, TIMBRE), P("UNIW", 20, TIMBRE), P("SUB1", 30, TIMBRE), P("SUB2", 0, TIMBRE),
    P("PW", 64, TIMBRE), P("PWAD", 20, MOD), C("PWRS", 32, MOD, ch_offon), P("TUNE", 64, PITCH),
};
static const struct voice_desc v_swave_pulse = {
    "SW-PULSE", FAM_MNM, N(p_swave_pulse), MM, 11, p_swave_pulse, &mnmd_voice_swave_pulse.v, 3.4326f,
};
/* The engine's interval steps: tempered semitones, and just ratios beside
 * them (3:2 = +7.02 semitones). */
static const char *const l_ens[] = {
    "-12", "-11", "-10", "-9", "-8", "-3:2", "-7", "-6", "-5", "-4:3", "-4", "-5:4", "-6:5", "-3", "-2", "-1",
    "OFF", "+1", "+2", "+3", "+6:5", "+5:4", "+4", "+4:3", "+5", "+6", "+7", "+3:2", "+8", "+9", "+10", "+11",
    "+12",
};
static const struct param_choice ch_ens = { 33, 0, l_ens, mnmd_swave_ens_step };
static const struct voice_param p_swave_ens[] = {
    C("PCH2", 84, PITCH, ch_ens), C("PCH3", 99, PITCH, ch_ens), C("PCH4", 62, PITCH, ch_ens), P("WAVE", 0, TIMBRE),
    P("PW", 64, TIMBRE), P("CHRL", 40, MOD), P("CHRW", 40, MOD), P("TUNE", 64, PITCH),
};
static const struct voice_desc v_swave_ens = {
    "SW-ENS", FAM_MNM, N(p_swave_ens), MM, 13, p_swave_ens, &mnmd_voice_swave_ens.v, 1.2874f,
};
static const struct voice_param p_fmpstat[] = {
    C("1FRQ", 82, MOD, ch_ratio), P("1FIN", 64, PITCH), P("1ENV", 92, ENV), P("1FB", 0, MOD),
    C("2FRQ", 114, MOD, ch_ratio), P("2VOL", 24, MOD), P("TONE", 80, TIMBRE), P("TUNE", 64, PITCH),
};
static const struct voice_desc v_fmpstat = {
    "FM-STAT", FAM_MNM, N(p_fmpstat), MM, 9, p_fmpstat, &mnmd_voice_fmpstat.v, 1.4103f,
};
static const struct voice_param p_fmppar[] = {
    C("1FRQ", 82, MOD, ch_ratio), P("1ENV", 48, ENV), C("2FRQ", 103, MOD, ch_ratio), P("2ENV", 90, ENV),
    C("3FRQ", 114, MOD, ch_ratio), P("3ENV", 104, ENV), P("TONE", 72, TIMBRE), P("TUNE", 64, PITCH),
};
static const struct voice_desc v_fmppar = {
    "FM-PAR", FAM_MNM, N(p_fmppar), MM, 10, p_fmppar, &mnmd_voice_fmppar.v, 1.2833f,
};
static const struct voice_param p_fmpdyn[] = {
    P("1FRQ", 64, MOD), P("1FEN", 64, ENV), P("1VOL", 40, MOD), P("1VEN", 92, ENV),
    P("2FRQ", 64, MOD), P("2ENV", 88, ENV), P("2FB", 0, MOD), P("TUNE", 64, PITCH),
};
static const struct voice_desc v_fmpdyn = {
    "FM-DYN", FAM_MNM, N(p_fmpdyn), MM, 9, p_fmpdyn, &mnmd_voice_fmpdyn.v, 1.3177f,
};
static const struct voice_param p_vo_6[] = {
    P("VOC1", 120, TIMBRE), P("VOC2", 58, TIMBRE), C("VSW", 95, MOD, ch_offon), P("VOIC", 0, TIMBRE),
    C("CONS", 3, TIMBRE, ch_cons), P("CLEN", 30, ENV), P("CVOL", 80, LEVEL), P("TUNE", 64, PITCH),
};
static const struct voice_desc v_vo_6 = {
    "VO-6", FAM_MNM, N(p_vo_6), MM, 9, p_vo_6, &mnmd_voice_vo_6.v, 4.1901f,
};
static const struct voice_param p_gnd_sin[] = {
    P("TUNE", 64, PITCH),
};
static const struct voice_desc v_gnd_sin = {
    "GND-SIN", FAM_MNM, N(p_gnd_sin), MM, 6, p_gnd_sin, &mnmd_voice_gnd_sin.v, 0.9639f,
};
static const struct voice_param p_gnd_nois[] = {
    P("ST", 0, LEVEL), P("RED", 0, TIMBRE), C("STON", 32, MOD, ch_offon), P("TUNE", 64, PITCH),
    P("RES", 0, TIMBRE),
};
static const struct voice_desc v_gnd_nois = {
    "GND-NOIS", FAM_MNM, N(p_gnd_nois), MM, 10, p_gnd_nois, &mnmd_voice_gnd_nois.v, 2.7998f,
};

unsigned mdkit_noise_salt;

const struct voice_desc *const voices[VOICE_COUNT] = {
    0,
    &v_trx_bd,
    &v_trx_sd,
    &v_trx_xt,
    &v_trx_cp,
    &v_trx_rs,
    &v_trx_cb,
    &v_trx_ch,
    &v_trx_oh,
    &v_trx_cy,
    &v_trx_ma,
    &v_trx_cl,
    &v_trx_xc,
    &v_trx_b2,
    &v_efm_bd,
    &v_efm_sd,
    &v_efm_xt,
    &v_efm_cp,
    &v_efm_rs,
    &v_efm_cb,
    &v_efm_hh,
    &v_efm_cy,
    &v_pi_bd,
    &v_pi_sd,
    &v_pi_mt,
    &v_pi_ml,
    &v_pi_ma,
    &v_pi_rs,
    &v_pi_rc,
    &v_pi_cc,
    &v_pi_hh,
    &v_gnd_sn,
    &v_gnd_ns,
    &v_gnd_im,
    &v_swave_saw,
    &v_sid_6581,
    &v_swave_pulse,
    &v_swave_ens,
    &v_fmpstat,
    &v_fmppar,
    &v_fmpdyn,
    &v_vo_6,
    &v_gnd_sin,
    &v_gnd_nois,
};

/* Picker order: by instrument, as ui.c machine_grid. */
static const uint8_t fam_trx[] = { 1, 13, 2, 3, 12, 4, 5, 6, 11, 10, 7, 8, 9 };
static const uint8_t fam_efm[] = { 14, 15, 16, 17, 18, 19, 20, 21 };
static const uint8_t fam_pi[] = { 22, 23, 24, 25, 27, 26, 30, 28, 29 };
static const uint8_t fam_gnd[] = { 31, 32, 33 };
static const uint8_t fam_mnm[] = { 34, 35, 36, 37, 38, 39, 40, 41, 42, 43 };

const struct family_desc families[FAMILY_COUNT] = {
    [FAM_TRX] = { "TRX", N(fam_trx), fam_trx },
    [FAM_EFM] = { "EFM", N(fam_efm), fam_efm },
    [FAM_PI] = { "PI", N(fam_pi), fam_pi },
    [FAM_GND] = { "GND", N(fam_gnd), fam_gnd },
    [FAM_MNM] = { "MNM", N(fam_mnm), fam_mnm },
};
