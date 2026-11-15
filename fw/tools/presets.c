/*
 * Writes the example presets as prstNNN.yml (src/yml.c) into OUTDIR, a
 * folder for the card root (e.g. "MACHINES DEMO"). Built and run by
 * tools/presets.sh with the vendored mdkit voices, so machine controls are
 * addressed by their names and start from their defaults.
 * usage: presets OUTDIR
 */
#include "presetkit.h"

static void machines(const char *name, const int mach[CHANNELS], const uint8_t *mode)
{
    kit(name);
    for (int c = 0; c < CHANNELS; c++) {
        m(c + 1, mach[c]);
        if (mode) {
            p.ch[c].mode = mode[c];
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: presets OUTDIR\n");
        return 2;
    }
    outdir = argv[1];

    {
        static const int trx[CHANNELS] = { TRX_BD, TRX_SD, TRX_CP, TRX_RS, TRX_CH, TRX_OH, TRX_CY, TRX_CB };
        static const int efm[CHANNELS] = { EFM_BD, EFM_SD, EFM_XT, EFM_CP, EFM_RS, EFM_CB, EFM_HH, EFM_CY };
        static const int pi[CHANNELS] = { PI_SD, PI_RS, PI_MT, PI_BD, PI_ML, PI_CC, PI_HH, PI_RC };
        static const int lnk[CHANNELS] = { TRX_BD, EFM_BD, TRX_SD, EFM_SD, TRX_CH, EFM_HH, TRX_OH, TRX_CP };
        static const uint8_t lnk_mode[CHANNELS] = {
            MODE_MASTER, MODE_LINK, MODE_MASTER, MODE_CYCLE,
            MODE_MASTER, MODE_CYCLE, MODE_CYCLE, MODE_MASTER
        };
        static const int sd8[CHANNELS] = { PI_SD, PI_SD, PI_SD, PI_SD, PI_SD, PI_SD, PI_SD, PI_SD };
        static const uint8_t all_link[CHANNELS] = {
            MODE_MASTER, MODE_LINK, MODE_LINK, MODE_LINK, MODE_LINK, MODE_LINK, MODE_LINK, MODE_LINK
        };

        machines("TRX KIT", trx, NULL);
        save(1);
        machines("EFM KIT", efm, NULL);
        save(2);
        /* Every PI voice but the cheap PI-MA: a near worst case for CPU. */
        machines("PI HEAVY", pi, NULL);
        save(3);
        /* 1+2 layered kick, 3 snare cycling with 4, 5 hat cycling 6/7. */
        machines("LINKS", lnk, lnk_mode);
        save(4);
        /* The CPU worst case: gate 1 fires eight PI-SDs. */
        machines("PI-SD X8", sd8, all_link);
        save(5);
    }

    kit("808 STYLE");
    m(1, TRX_BD); k(1, "PTCH", 40); k(1, "DEC", 110);
    m(2, TRX_SD); k(2, "SNAP", 90); k(2, "TONE", 40);
    m(3, TRX_CP);
    m(4, TRX_RS);
    m(5, TRX_CH); k(5, "DEC", 10);
    m(6, TRX_OH); k(6, "DEC", 70);
    m(7, TRX_CY);
    m(8, TRX_CB);
    save(10);

    kit("TECHNO");
    m(1, TRX_BD); k(1, "PTCH", 52); k(1, "DEC", 70); k(1, "RAMP", 40); k(1, "RDEC", 30); k(1, "CLIP", 60);
    m(2, TRX_CP); k(2, "ROOM", 90);
    m(3, TRX_CH); k(3, "DEC", 6);
    m(4, TRX_OH); k(4, "DEC", 40);
    m(5, EFM_RS);
    m(6, TRX_CY);
    m(7, TRX_XT); k(7, "PTCH", 40);
    m(8, EFM_CB);
    save(11);

    kit("DNB");
    m(1, TRX_B2); k(1, "DEC", 40); k(1, "TICK", 110);
    m(2, TRX_SD); k(2, "PTCH", 80); k(2, "DEC", 20); k(2, "SNAP", 127); k(2, "TONE", 60);
    m(3, TRX_CH); k(3, "DEC", 4); k(3, "HPF", 60);
    m(4, TRX_OH); k(4, "DEC", 30);
    m(5, TRX_RS);
    m(6, EFM_HH);
    m(7, TRX_CY);
    m(8, TRX_CL);
    save(12);

    kit("EFM PERC");
    m(1, EFM_BD); k(1, "MOD", 60); k(1, "MFB", 50);
    m(2, EFM_SD);
    m(3, EFM_XT);
    m(4, EFM_CP);
    m(5, EFM_RS); k(5, "SMOD", 50);
    m(6, EFM_CB); k(6, "FB", 90);
    m(7, EFM_HH); k(7, "TREM", 60);
    m(8, EFM_CY);
    save(13);

    kit("PI ACOUSTIC");
    m(1, PI_BD); m(2, PI_SD); m(3, PI_MT); m(4, PI_ML);
    m(5, PI_RS); m(6, PI_HH); m(7, PI_CC); m(8, PI_RC);
    save(14);

    kit("METALS");
    m(1, TRX_CY); m(2, TRX_CB); m(3, EFM_CB); k(3, "PTCH", 80);
    m(4, EFM_HH); m(5, EFM_CY); k(5, "PTCH", 40);
    m(6, PI_HH); m(7, PI_CC); m(8, PI_RC);
    save(15);

    kit("TOMS");
    for (int c = 1; c <= 8; c++) {
        m(c, TRX_XT);
        k(c, "PTCH", 10 + (c - 1) * 15);
    }
    save(16);

    kit("LO-FI");
    m(1, TRX_BD); m(2, TRX_SD); m(3, TRX_CP); m(4, TRX_RS);
    m(5, TRX_CH); m(6, TRX_OH); m(7, TRX_CY); m(8, TRX_CB);
    for (int c = 0; c < CHANNELS; c++) {
        p.ch[c].bits = (int16_t)(60 + 10 * (c % 4));   /* 6-9 bits */
        p.ch[c].pitch = -200;                         /* two semitones down */
    }
    save(17);

    /* 1+2+3 one layered kick, 4+5 snare, 6+7 clap (Link). */
    kit("LAYERS");
    m(1, TRX_BD); m(2, EFM_BD); p.ch[1].mode = MODE_LINK;
    m(3, PI_BD); p.ch[2].mode = MODE_LINK;
    m(4, TRX_SD); m(5, EFM_SD); p.ch[4].mode = MODE_LINK;
    m(6, TRX_CP); m(7, TRX_CL); p.ch[6].mode = MODE_LINK;
    m(8, TRX_CH);
    save(18);

    /* Gate 2 rotates three snares, gate 5 four hats (Cycle). */
    kit("CYCLES");
    m(1, TRX_BD);
    m(2, TRX_SD); m(3, EFM_SD); m(4, PI_SD);
    p.ch[2].mode = MODE_CYCLE; p.ch[3].mode = MODE_CYCLE;
    m(5, TRX_CH); m(6, TRX_OH); m(7, EFM_HH); m(8, PI_HH);
    p.ch[5].mode = MODE_CYCLE; p.ch[6].mode = MODE_CYCLE; p.ch[7].mode = MODE_CYCLE;
    save(19);

    kit("STEREO KIT");
    m(1, TRX_BD); m(2, TRX_SD); m(3, TRX_CH); m(4, TRX_OH);
    m(5, TRX_XT); k(5, "PTCH", 30); m(6, TRX_XT); k(6, "PTCH", 70);
    m(7, TRX_CY); m(8, EFM_CB);
    {
        static const int16_t pan[CHANNELS] = { 0, 0, -60, 60, -40, 40, 70, -70 };
        for (int c = 0; c < CHANNELS; c++) {
            p.ch[c].pan = pan[c];
        }
    }
    save(20);

    /* Each channel: CV A = pitch (1 V/oct), CV B = decay (+-5 V full range). */
    kit("CV PLAY");
    m(1, TRX_BD); m(2, TRX_SD); m(3, TRX_XT); m(4, EFM_XT);
    m(5, TRX_CH); m(6, EFM_CB); m(7, PI_MT); m(8, PI_ML);
    for (int c = 1; c <= 8; c++) {
        cv(c, D_PITCH, CV_REL_A, 50);
        cv(c, D_CTRL + control(c, "DEC"), CV_REL_B, 100);
    }
    save(21);

    return 0;
}
