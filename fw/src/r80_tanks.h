/* The four tank listings of section 5, for r80.c to include twice: with
 * TANK_FADE 0 (plain) and 1 (reads and writes of the steps whose offsets
 * change scaled by the fade gain, 6.4). TANK(name) names the variant.
 * Each runs one sample in the listing's order; the one-pole states that
 * the listings keep at M0[n-1..n-8] stay in variables (crossovers five
 * bits finer, Q22). */

#if TANK_FADE
#define FG(s)           ((int32_t)fade_gain[fade_class[s]])
#define RD(m, s)        ((m##p[ix[s]] * FG(s)) >> 8)
#define WR(m, s, v)     put(m, ix[s] + f, (sat18(q17(v)) * FG(s)) >> 8)
#else
#define RD(m, s)        (m##p[ix[s]])
#define WR(m, s, v)     put(m, ix[s] + f, sat18(q17(v)))
#endif
#define WRF(m, off, v)  put(m, i - (off), sat18(q17(v)))
#define RDF(m, off)     (m[(i - (off)) & (RING - 1u)])

/* Concert Hall (5.3). */
static void TANK(hall)(unsigned count)
{
    const int16_t *k = st.k, *kr = st.kret;
    const uint32_t *ix = st.ix, i0 = st.i;
    int32_t lp1 = st.s[0], d1 = st.s[1], lp2 = st.s[2], d2 = st.s[3], x123 = st.s[4], x124 = st.s[5];
    int32_t sl = st.sl, sr = st.sr;

    for (unsigned f = 0; f < count; f++) {
        const int32_t *m0p = m0 + f, *m1p = m1 + f, *m2p = m2 + f;
        const uint32_t i = i0 + f;
        events();
#if TANK_FADE
        fade_tick();
#endif
        const int32_t k1 = 255 - st.code[2], k7 = 255 - st.code[3], k115 = 255 - st.code[0],
                      k121 = 255 - st.code[1];

        int32_t x3 = RD(m0, 3), x4 = RD(m0, 4);
        WR(m0, 5, x123 * (256 - k1) + x124 * k1);
        int32_t x9 = RD(m0, 9);
        WR(m0, 10, x3 * (256 - k7) + x4 * k7);
        lp1 = lp_step(lp1, x9, k[11], k[9]);
        int32_t x12 = RD(m0, 12), x13 = RD(m0, 13), x15 = RD(m0, 15), x16 = RD(m0, 16), x17 = RD(m1, 17);
        int32_t x18 = RD(m0, 18), x19 = RD(m0, 19), x20 = RD(m0, 20), x21 = RD(m0, 21), x22 = RD(m2, 22);
        int32_t x24 = RD(m1, 24), x26 = RD(m2, 26);
        int32_t x31 = RD(m1, 31), x32 = RD(m1, 32), x33 = RD(m1, 33);
        int32_t x38 = RD(m0, 38);
        int32_t u = x9 * k[27] + ((lp1 * k[29] + 16) >> 5) + x31 * k[31] + x32 * k[32] + x33 * k[33];
        d1 = sat18(q17(q17(u) * k[36] + d1 * k[34]));
        int32_t w = d1 * 256 + x38 * k[38];
        WRF(m0, 2100, w);
        int32_t x42 = RD(m0, 42), x44 = RD(m0, 44);
        w = q17(w) * k[40] + x38 * 256 + x42 * k[42];
        WR(m0, 45, w);
        int32_t x46 = RD(m0, 46);
        WR(m0, 47, q17(w) * k[44] + x42 * 256);
        int32_t x48 = RD(m0, 48), x50 = RD(m0, 50), x51 = RD(m0, 51);
        /* Nested allpass block 1: outer (k46/k52) around inner (k48/k50). */
        int32_t wo = x44 * 256 + x46 * k[46], wi = wo + x48 * k[48];
        WRF(m0, 255, wi);
        WR(m0, 53, q17(wi) * k[50] + x51 * 256);
        int32_t x54 = RD(m0, 54);
        WR(m0, 55, q17(wo) * k[52] + x46 * 256);
        int32_t x56 = RD(m0, 56), x58 = RD(m0, 58), x61 = RD(m0, 61);
        /* Block 2: three deep (k54/k64, k56/k62, k58/k60). */
        int32_t p = x50 * 256 + x54 * k[54], q = p + x56 * k[56], r = q + x58 * k[58];
        WRF(m0, 767, r);
        WR(m0, 63, q17(r) * k[60] + x61 * 256);
        int32_t x64 = RD(m0, 64), x65 = RD(m0, 65);
        WR(m0, 66, q17(q) * k[62] + x56 * 256);
        WR(m0, 67, q17(p) * k[64] + x65 * 256);
        lp2 = lp_step(lp2, x64, k[68], k[66]);
        int32_t x75 = RD(m2, 75), x76 = RD(m2, 76), x77 = RD(m2, 77), x79 = RD(m0, 79);
        u = x64 * k[71] + ((lp2 * k[73] + 16) >> 5) + x75 * k[75] + x76 * k[76] + x77 * k[77];
        d2 = sat18(q17(q17(u) * k[80] + d2 * k[78]));
        w = d2 * 256 + x79 * k[83];
        WR(m0, 86, w);
        int32_t x87 = RD(m0, 87), x89 = RD(m0, 89);
        w = q17(w) * k[85] + x79 * 256 + x87 * k[87];
        WR(m0, 90, w);
        int32_t x91 = RD(m0, 91);
        WR(m0, 92, q17(w) * k[89] + x87 * 256);
        int32_t x93 = RD(m0, 93), x95 = RD(m0, 95), x96 = RD(m0, 96);
        /* Block 3 (k91/k97 around k93/k95). */
        wo = x89 * 256 + x91 * k[91];
        wi = wo + x93 * k[93];
        WRF(m0, 1279, wi);
        WR(m0, 98, q17(wi) * k[95] + x96 * 256);
        int32_t x99 = RD(m0, 99);
        WR(m0, 100, q17(wo) * k[97] + x91 * 256);
        int32_t x101 = RD(m0, 101), x103 = RD(m0, 103), x106 = RD(m0, 106);
        /* Block 4 (k99/k109, k101/k107, k103/k105). */
        p = x95 * 256 + x99 * k[99];
        q = p + x101 * k[101];
        r = q + x103 * k[103];
        WRF(m0, 1791, r);
        WR(m0, 108, q17(r) * k[105] + x106 * 256);
        int32_t x109 = RD(m0, 109), x110 = RD(m0, 110);
        WR(m0, 111, q17(q) * k[107] + x101 * 256);
        int32_t x112 = RD(m0, 112);
        WR(m0, 113, q17(p) * k[109] + x110 * 256);
        WRF(m1, 26626, sr * 192);
        int32_t x117 = RD(m0, 117), x118 = RD(m0, 118);
        WR(m0, 119, x109 * (256 - k115) + x112 * k115);
        WRF(m2, 26626, sl * 192);
        x123 = RD(m0, 123);
        x124 = RD(m0, 124);
        WR(m0, 125, x117 * (256 - k121) + x118 * k121);
        ret_q[f][0] = sat18(q17(x18 * kr[0] + x19 * kr[1] + x20 * kr[2] + x21 * kr[3] + x22 * kr[4] + x26 * kr[5]));
        ret_q[f][1] = sat18(q17(x12 * kr[6] + x13 * kr[7] + x15 * kr[8] + x16 * kr[9] + x17 * kr[10] + x24 * kr[11]));
        sl = send_q[f][0];
        sr = send_q[f][1];
    }
    st.i = (i0 + count) & (RING - 1u);
    st.s[0] = lp1;
    st.s[1] = d1;
    st.s[2] = lp2;
    st.s[3] = d2;
    st.s[4] = x123;
    st.s[5] = x124;
    st.sl = sl;
    st.sr = sr;
}

/* Chamber (5.1). */
static void TANK(chamber)(unsigned count)
{
    const int16_t *k = st.k;
    const uint32_t *ix = st.ix, i0 = st.i;
    /* w27 and w116 (damping), w31 and w121 (crossovers, Q22), w116 of the
     * sample before last. */
    int32_t d1 = st.s[0], xo1 = st.s[1], d2 = st.s[2], xo2 = st.s[3], d2p = st.s[4];
    int32_t sl = st.sl, sr = st.sr;

    for (unsigned f = 0; f < count; f++) {
        const int32_t *m0p = m0 + f, *m1p = m1 + f, *m2p = m2 + f;
        const uint32_t i = i0 + f;
        events();
#if TANK_FADE
        fade_tick();
#endif
        int32_t x0 = RD(m0, 0), x2 = RD(m0, 2);
        int32_t w = x0 * k[0] + ((xo2 * k[122] + 16) >> 5) + d2p * k[120];
        WR(m0, 3, w);
        WR(m0, 5, q17(w) * k[2] + x0 * 256);
        int32_t x6 = RD(m0, 6), x8 = RD(m0, 8);
        w = x2 * k[4] + x6 * k[6];
        WR(m0, 9, w);
        WR(m0, 11, q17(w) * k[8] + x6 * 256);
        int32_t x13 = RD(m1, 13), x14 = RD(m1, 14), x15 = RD(m0, 15), x17 = RD(m0, 17);
        w = x13 * k[13] + x14 * k[14] + x15 * k[15] + x8 * k[11];
        WR(m0, 18, w);
        WR(m0, 20, q17(w) * k[17] + x15 * 256);
        int32_t x24 = RD(m1, 24), x25 = RD(m1, 25);
        int32_t d1n = sat18(q17(x17 * k[20] + d1 * k[22] + x24 * k[24] + x25 * k[25]));
        xo1 = lp_step(xo1, d1, k[26], k[28]);
        int32_t x34 = RD(m0, 34);
        w = d1 * k[30] + ((xo1 * k[32] + 16) >> 5) + x34 * k[34];
        d1 = d1n;
        WR(m0, 37, w);
        int32_t x38 = RD(m0, 38);
        WR(m0, 39, q17(w) * k[36] + x34 * 256);
        int32_t x40 = RD(m0, 40);
        w = x38 * k[38] + x40 * k[40];
        WR(m0, 43, w);
        int32_t x44 = RD(m0, 44);
        WR(m0, 45, q17(w) * k[42] + x40 * 256);
        int32_t x47 = RD(m2, 47), x48 = RD(m2, 48), x49 = RD(m0, 49), x50 = RD(m0, 50), x51 = RD(m0, 51);
        w = x44 * k[45] + x47 * k[47] + x48 * k[48] + x49 * k[49];
        WR(m0, 52, w);
        int32_t x53 = RD(m0, 53);
        WR(m0, 54, q17(w) * k[51] + x49 * 256);
        int32_t r = -96 * x50 + 128 * x53 - 128 * RD(m0, 55) - 112 * RD(m0, 56) - 96 * RD(m0, 57) +
                    128 * RD(m0, 58) + 96 * RD(m0, 59) - 128 * RD(m0, 60) - 112 * RD(m0, 61) + 96 * RD(m0, 62) +
                    128 * RD(m0, 63) + 96 * RD(m0, 64) + 128 * RD(m0, 65) - 112 * RD(m0, 66) + 96 * RD(m0, 67) +
                    RD(m1, 68) * k[68];
        int32_t x69 = RD(m1, 69);
        int32_t l = 112 * x51 + 128 * RD(m0, 71) + 96 * RD(m0, 73) + 128 * RD(m0, 74) - 96 * RD(m0, 75) +
                    128 * RD(m0, 76) - 112 * RD(m0, 77) - 96 * RD(m0, 78) + 128 * RD(m0, 79) - 112 * RD(m0, 80) +
                    96 * RD(m0, 81) - 128 * RD(m0, 82) + 96 * RD(m0, 83) + 128 * RD(m0, 84) - 96 * RD(m0, 85) +
                    RD(m2, 86) * k[86];
        int32_t x87 = RD(m2, 87);
        ret_q[f][0] = sat18(q17(q17(l) * k[88] + x87 * k[87]));
        ret_q[f][1] = sat18(q17(q17(r) * k[70] + x69 * k[69]));
        int32_t x89 = RD(m0, 89), x91 = RD(m1, 91);
        w = x89 * k[89] + x91 * k[91] + sr * 256;
        WRF(m0, 33, w);
        int32_t x95 = RD(m0, 95);
        w = q17(w) * k[93] + x89 * 256 + x95 * k[95];
        WR(m0, 98, w);
        int32_t x99 = RD(m0, 99);
        WRF(m1, 0, q17(w) * k[97] + x95 * 256);
        int32_t x101 = RD(m2, 101);
        w = x101 * k[101] + x99 * k[99] + sl * 256;
        WR(m0, 104, w);
        int32_t x105 = RD(m0, 105);
        w = q17(w) * k[103] + x99 * 256 + x105 * k[105];
        WR(m0, 108, w);
        int32_t x109 = RD(m0, 109);
        WRF(m2, 0, q17(w) * k[107] + x105 * 256);
        int32_t x113 = RD(m2, 113), x114 = RD(m2, 114);
        int32_t d2n = sat18(q17(x109 * k[109] + d2 * k[111] + x113 * k[113] + x114 * k[114]));
        xo2 = lp_step(xo2, d2, k[116], k[118]);
        d2p = d2;
        d2 = d2n;
        sl = send_q[f][0];
        sr = send_q[f][1];
    }
    st.i = (i0 + count) & (RING - 1u);
    st.s[0] = d1;
    st.s[1] = xo1;
    st.s[2] = d2;
    st.s[3] = xo2;
    st.s[4] = d2p;
    st.sl = sl;
    st.sr = sr;
}

/* Plate (5.2; Chorus+Rvb runs it too). */
static void TANK(plate)(unsigned count)
{
    const int16_t *k = st.k;
    const uint32_t *ix = st.ix, i0 = st.i;
    /* w21 and w119 (damping), w25 and w86 (crossovers, Q22), w126 and X123
     * of the previous sample. */
    int32_t d1 = st.s[0], xo1 = st.s[1], d2 = st.s[2], xo2 = st.s[3], w126 = st.s[4], x123 = st.s[5];
    int32_t sl = st.sl, sr = st.sr;

    for (unsigned f = 0; f < count; f++) {
        const int32_t *m0p = m0 + f, *m1p = m1 + f, *m2p = m2 + f;
        const uint32_t i = i0 + f;
        events();
#if TANK_FADE
        fade_tick();
#endif
        int32_t x0 = RD(m0, 0);
        WR(m0, 1, w126 * k[125] + x123 * 256);
        int32_t x2 = RD(m1, 2), x3 = RD(m0, 3), x5 = RD(m0, 5);
        int32_t w = x0 * k[0] + x2 * k[2] + x3 * k[3];
        WR(m0, 6, w);
        WR(m0, 8, q17(w) * k[5] + x3 * 256);
        int32_t x10 = RD(m1, 10), x11 = RD(m0, 11), x13 = RD(m0, 13);
        w = x10 * k[10] + x11 * k[11] + x5 * k[8];
        WR(m0, 14, w);
        WR(m0, 16, q17(w) * k[13] + x11 * 256);
        int32_t d1o = d1;
        d1 = sat18(q17(x13 * k[16] + d1 * k[18]));
        xo1 = lp_step(xo1, d1o, k[20], k[22]);
        int32_t x28 = RD(m2, 28), x29 = RD(m0, 29);
        w = d1o * k[24] + ((xo1 * k[26] + 16) >> 5) + x28 * k[28] + x29 * k[29];
        WR(m0, 32, w);
        int32_t x33 = RD(m0, 33);
        WR(m0, 34, q17(w) * k[31] + x29 * 256);
        int32_t x35 = RD(m2, 35), x36 = RD(m0, 36), x38 = RD(m0, 38);
        w = x33 * k[33] + x35 * k[35] + x36 * k[36];
        WR(m0, 39, w);
        WR(m0, 41, q17(w) * k[38] + x36 * 256);
        int32_t x43 = RD(m2, 43), x44 = RD(m0, 44), x45 = RD(m0, 45), x46 = RD(m0, 46);
        w = x38 * k[41] + x43 * k[43] + x44 * k[44];
        WR(m0, 47, w);
        int32_t x48 = RD(m0, 48);
        WR(m0, 49, q17(w) * k[46] + x44 * 256);
        int32_t r = -96 * x45 + 128 * x48 - 128 * RD(m0, 50) - 112 * RD(m0, 51) - 96 * RD(m0, 52) +
                    128 * RD(m0, 53) - 128 * RD(m0, 54) - 112 * RD(m0, 55) + 96 * RD(m0, 56) + 128 * RD(m0, 57) +
                    96 * RD(m0, 58) + 128 * RD(m0, 59) - 96 * RD(m0, 60) + RD(m1, 61) * k[61];
        int32_t x62 = RD(m1, 62);
        int32_t l = 96 * x46 + 128 * RD(m0, 64) + 128 * RD(m0, 66) - 96 * RD(m0, 67) + 128 * RD(m0, 68) -
                    112 * RD(m0, 69) - 96 * RD(m0, 70) + 128 * RD(m0, 71) - 112 * RD(m0, 72) + 96 * RD(m0, 73) -
                    128 * RD(m0, 74) + 96 * RD(m0, 75) + 128 * RD(m0, 76) - 96 * RD(m0, 77) + RD(m2, 78) * k[78];
        int32_t x79 = RD(m2, 79);
        ret_q[f][0] = sat18(q17(q17(l) * k[80] + x79 * k[79]));
        ret_q[f][1] = sat18(q17(q17(r) * k[63] + x62 * k[62]));
        int32_t x84 = RD(m0, 84);
        int32_t xo2n = lp_step(xo2, d2, k[81], k[83]);
        int32_t x88 = RD(m1, 88);
        w = x84 * k[86] + x88 * k[88] + sr * 256;
        WRF(m0, 20, w);
        int32_t x92 = RD(m0, 92);
        w = q17(w) * k[90] + x84 * 256 + x92 * k[92];
        WR(m0, 95, w);
        int32_t x96 = RD(m0, 96);
        w = q17(w) * k[94] + x92 * 256 + x96 * k[96];
        WR(m0, 99, w);
        int32_t x100 = RD(m0, 100);
        WRF(m1, 0, q17(w) * k[98] + x96 * 256);
        int32_t x102 = RD(m2, 102);
        w = x100 * k[100] + x102 * k[102] + sl * 256;
        WR(m0, 105, w);
        int32_t x106 = RD(m0, 106);
        w = q17(w) * k[104] + x100 * 256 + x106 * k[106];
        WR(m0, 109, w);
        int32_t x110 = RD(m0, 110);
        w = q17(w) * k[108] + x106 * 256 + x110 * k[110];
        WR(m0, 113, w);
        int32_t x114 = RD(m0, 114);
        WRF(m2, 0, q17(w) * k[112] + x110 * 256);
        int32_t d2o = d2;
        d2 = sat18(q17(x114 * k[114] + d2 * k[116]));
        int32_t x122 = RD(m1, 122);
        x123 = RD(m0, 123);
        xo2 = xo2n;
        w = d2o * k[118] + x122 * k[122] + x123 * k[123] + ((xo2 * k[120] + 16) >> 5);
        WR(m0, 126, w);
        w126 = sat18(q17(w));
        sl = send_q[f][0];
        sr = send_q[f][1];
    }
    st.i = (i0 + count) & (RING - 1u);
    st.s[0] = d1;
    st.s[1] = xo1;
    st.s[2] = d2;
    st.s[3] = xo2;
    st.s[4] = w126;
    st.s[5] = x123;
    st.sl = sl;
    st.sr = sr;
}

/* Inverse (5.4). */
static void TANK(inverse)(unsigned count)
{
    const int16_t *k = st.k;
    const uint32_t *ix = st.ix, i0 = st.i;
    /* w27 and w117 (damping), w31 and w122 (crossovers, Q22), w117 of the
     * sample before last. */
    int32_t d1 = st.s[0], xo1 = st.s[1], d2 = st.s[2], xo2 = st.s[3], d2p = st.s[4];
    int32_t sl = st.sl, sr = st.sr;

    for (unsigned f = 0; f < count; f++) {
        const int32_t *m0p = m0 + f, *m1p = m1 + f, *m2p = m2 + f;
        const uint32_t i = i0 + f;
        events();
#if TANK_FADE
        fade_tick();
#endif
        int32_t x0 = RD(m0, 0), x1 = RD(m0, 1);
        int32_t w = -128 * x0 + ((xo2 * k[123] + 16) >> 5) + d2p * (256 + k[121]);
        WR(m0, 3, w);
        WR(m0, 5, q17(w) * 128 + x0 * 256);
        int32_t x6 = RD(m0, 6), x7 = RD(m1, 7), x8 = RD(m1, 8);
        w = x1 * (256 + k[4]) - 128 * x6;
        WR(m0, 9, w);
        WR(m0, 11, q17(w) * 128 + x6 * 256);
        int32_t x13 = RD(m1, 13), x14 = RD(m1, 14), x15 = RD(m0, 15), x19 = RD(m0, 19);
        w = q17(x13 * k[13] + x14 * k[14] + x7 * k[11] + x8 * k[12]) * k[17] - 128 * x15;
        WR(m0, 20, w);
        WR(m0, 22, q17(w) * 128 + x15 * 256);
        int32_t d1o = d1;
        d1 = sat18(q17(x19 * k[22] + d1 * k[24]));
        xo1 = lp_step(xo1, d1o, k[26], k[28]);
        int32_t x34 = RD(m0, 34), x35 = RDF(m2, 32769);
        w = d1o * (256 + k[30]) + ((xo1 * k[32] + 16) >> 5) - 128 * x34;
        WR(m0, 37, w);
        int32_t x38 = RD(m0, 38), x39 = RD(m2, 39), x40 = RD(m0, 40);
        WR(m0, 41, q17(w) * 128 + x34 * 256);
        w = x38 * (256 + k[38]) - 128 * x40;
        WR(m0, 43, w);
        WR(m0, 45, q17(w) * 128 + x40 * 256);
        int32_t x46 = RD(m2, 46), x47 = RD(m2, 47), x49 = RD(m0, 49), x52 = RD(m0, 52), x53 = RD(m0, 53);
        w = q17(x35 * k[48] + x39 * k[45] + x46 * k[46] + x47 * k[47]) * k[51] - 128 * x49;
        WR(m0, 54, w);
        int32_t x55 = RD(m0, 55);
        WR(m0, 56, q17(w) * 128 + x49 * 256);
        int32_t r = -96 * x53 + 128 * x55 - 128 * RD(m0, 57) - 112 * RD(m0, 58) - 96 * RD(m0, 59) +
                    128 * RD(m0, 60) - 128 * RD(m0, 61) - 112 * RD(m0, 62) + 96 * RD(m0, 63) + 128 * RD(m0, 64) +
                    96 * RD(m0, 65) + 128 * RD(m0, 66) - 96 * RD(m0, 67) - 96 * RD(m0, 68) + RD(m1, 69) * k[69] +
                    RD(m1, 70) * k[70];
        int32_t l = 96 * x52 + 128 * RD(m0, 71) + 128 * RD(m0, 73) - 96 * RD(m0, 74) + 128 * RD(m0, 75) -
                    112 * RD(m0, 76) - 96 * RD(m0, 77) + 128 * RD(m0, 78) - 112 * RD(m0, 79) + 96 * RD(m0, 80) -
                    128 * RD(m0, 81) + 96 * RD(m0, 82) + 128 * RD(m0, 83) - 96 * RD(m0, 84) - 96 * RD(m0, 85) +
                    RD(m2, 86) * k[86] + RD(m2, 87) * k[87];
        ret_q[f][0] = sat18(q17(l));
        ret_q[f][1] = sat18(q17(r));
        int32_t x88 = RD(m0, 88);
        w = x88 * k[88] + sr * 256;
        WRF(m0, 20, w);
        int32_t x92 = RD(m0, 92);
        w = q17(w) * k[90] + x88 * 256 + x92 * k[92];
        WR(m0, 95, w);
        int32_t x96 = RD(m0, 96);
        w = q17(w) * k[94] + x92 * 256 + x96 * k[96];
        WR(m0, 99, w);
        int32_t x100 = RD(m0, 100);
        WRF(m1, 0, q17(w) * k[98] + x96 * 256);
        w = x100 * k[100] + sl * 256;
        WR(m0, 103, w);
        int32_t x104 = RD(m0, 104);
        w = q17(w) * k[102] + x100 * 256 + x104 * k[104];
        WR(m0, 107, w);
        int32_t x108 = RD(m0, 108), x110 = RD(m0, 110);
        w = q17(w) * k[106] + x104 * 256 + x108 * k[108];
        WR(m0, 111, w);
        WRF(m2, 32768, q17(w) * k[110] + x108 * 256);
        int32_t d2n = sat18(q17(x110 * k[112] + d2 * k[114]));
        xo2 = lp_step(xo2, d2, k[117], k[119]);
        d2p = d2;
        d2 = d2n;
        sl = send_q[f][0];
        sr = send_q[f][1];
    }
    st.i = (i0 + count) & (RING - 1u);
    st.s[0] = d1;
    st.s[1] = xo1;
    st.s[2] = d2;
    st.s[3] = xo2;
    st.s[4] = d2p;
    st.sl = sl;
    st.sr = sr;
}

#undef RD
#undef RDF
#undef WR
#undef WRF
#if TANK_FADE
#undef FG
#endif
