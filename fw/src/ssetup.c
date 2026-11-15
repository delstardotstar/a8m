#include "rec.h"
#include "cv.h"

/* Stock's 0:/ssetup.yml keys and values (Threshold: dB, Off, or a CV input
 * such as 1A); the Monitor* keys after MonitorR are ours. */

enum { K_DEST, K_ZONE, K_MODE, K_TIME, K_RATE, K_NAME, K_HPF, K_HOT, K_THRESH, K_SIDE, K_LEVEL, K_MONL, K_MONR,
       K_MONLEVEL, K_MONREV, K_MONDLY, K_COUNT };

static const struct { const char *key; int lo, hi; } keys[K_COUNT] = {
    { "Destination", 0, RDEST_EMPTY }, { "ZoneDestination", 0, 1 }, { "Mode", 0, RMODE_COUNT - 1 },
    { "Time", 0, RTIME_MAX }, { "Rate", 0, RRATE_192 }, { "Name", 0, RNAME_PRESET }, { "HPF", 0, 1 },
    { "ArmedHot", 0, 1 }, { "Threshold", RTHRESH_MIN, CV_INPUTS }, { "Side", 0, RSIDE_DUAL },
    { "Level", RLEVEL_MIN, RLEVEL_MAX }, { "MonitorL", 0, 1 }, { "MonitorR", 0, 1 },
    { "MonitorLevel", RMON_OFF, RMON_MAX }, { "MonitorReverb", 0, RMON_SEND_MAX },
    { "MonitorDelay", 0, RMON_SEND_MAX },
};

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static int key_is(const char *k, size_t n, const char *name)
{
    size_t i = 0;

    for (size_t j = 0; j < n; j++) {
        if (k[j] == ' ' || k[j] == '\t') {
            continue;
        }
        if (!name[i] || lower(k[j]) != lower(name[i])) {
            return 0;
        }
        i++;
    }
    return !name[i];
}

static int num_val(const char *v, size_t n, int *out)
{
    int neg = n && v[0] == '-', x = 0;
    size_t i = (size_t)neg;

    if (i == n) {
        return 0;
    }
    for (; i < n; i++) {
        if (v[i] < '0' || v[i] > '9' || x > 100000) {
            return 0;
        }
        x = x * 10 + v[i] - '0';
    }
    *out = neg ? -x : x;
    return 1;
}

/* Threshold: "Off", "1A".."8C" or dB. */
static int thresh_val(const char *v, size_t n, int *out)
{
    if (n == 3 && lower(v[0]) == 'o' && lower(v[1]) == 'f' && lower(v[2]) == 'f') {
        *out = 0;
        return 1;
    }
    if (n == 2 && v[0] >= '1' && v[0] <= '8' && lower(v[1]) >= 'a' && lower(v[1]) <= 'c') {
        *out = 1 + (v[0] - '1') * 3 + (lower(v[1]) - 'a');
        return 1;
    }
    return num_val(v, n, out) && *out < 0;
}

static void set(struct rec_setup *r, int i, int x)
{
    switch (i) {
    case K_DEST: r->dest = (uint8_t)x; break;
    case K_ZONE: r->zone_empty = (uint8_t)x; break;
    case K_MODE: r->mode = (uint8_t)x; break;
    case K_TIME: r->time = (int16_t)x; break;
    case K_RATE: r->rate = (uint8_t)x; break;
    case K_NAME: r->naming = (uint8_t)x; break;
    case K_HPF: r->hpf = (uint8_t)x; break;
    case K_HOT: r->hot = (uint8_t)x; break;
    case K_THRESH: r->thresh = (int16_t)x; break;
    case K_SIDE: r->side = (uint8_t)x; break;
    case K_LEVEL: r->level = (int8_t)x; break;
    case K_MONL: r->mon_l = (uint8_t)x; break;
    case K_MONR: r->mon_r = (uint8_t)x; break;
    case K_MONLEVEL: r->mon_level = (int8_t)x; break;
    case K_MONREV: r->mon_rev = (uint8_t)x; break;
    default: r->mon_dly = (uint8_t)x; break;
    }
}

static int get(const struct rec_setup *r, int i)
{
    const int v[K_COUNT] = { r->dest, r->zone_empty, r->mode, r->time, r->rate, r->naming, r->hpf, r->hot,
                             r->thresh, r->side, r->level, r->mon_l, r->mon_r, r->mon_level, r->mon_rev,
                             r->mon_dly };
    return v[i];
}

static void line(const char *k, size_t kn, const char *v, size_t vn, struct rec_setup *r)
{
    int x;

    for (int i = 0; i < K_COUNT; i++) {
        if (key_is(k, kn, keys[i].key)) {
            if ((i == K_THRESH ? thresh_val(v, vn, &x) : num_val(v, vn, &x)) && x >= keys[i].lo && x <= keys[i].hi) {
                set(r, i, x);
            }
            return;
        }
    }
}

void ssetup_parse(const char *buf, size_t len, struct rec_setup *r)
{
    size_t pos = 0;

    while (pos < len) {
        size_t a = pos, e, colon;

        while (pos < len && buf[pos] != '\n') {
            pos++;
        }
        e = pos++;
        for (colon = a; colon < e && buf[colon] != ':'; colon++) {
        }
        if (colon == e) {
            continue;
        }
        size_t vb = colon + 1, ve = e, ke = colon;
        while (vb < ve && is_ws(buf[vb])) {
            vb++;
        }
        while (ve > vb && is_ws(buf[ve - 1])) {
            ve--;
        }
        while (ke > a && is_ws(buf[ke - 1])) {
            ke--;
        }
        line(buf + a, ke - a, buf + vb, ve - vb, r);
    }
}

size_t ssetup_text(const struct rec_setup *r, char *out, size_t cap)
{
    static const char head[] = "# Sampling Setup\n";
    char t[512], *o = t;

    for (const char *s = head; *s; s++) {
        *o++ = *s;
    }
    for (int i = 0; i < K_COUNT; i++) {
        int v = get(r, i);
        char d[8];
        int n = 0;

        for (const char *s = keys[i].key; *s; s++) {
            *o++ = *s;
        }
        *o++ = ' ';
        *o++ = ':';
        *o++ = ' ';
        if (i == K_THRESH && v == 0) {
            *o++ = 'O';
            *o++ = 'f';
            *o++ = 'f';
        } else if (i == K_THRESH && v > 0) {
            *o++ = (char)('1' + (v - 1) / 3);
            *o++ = (char)('A' + (v - 1) % 3);
        } else {
            if (v < 0) {
                *o++ = '-';
                v = -v;
            }
            do {
                d[n++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            while (n) {
                *o++ = d[--n];
            }
        }
        *o++ = '\n';
    }
    size_t len = (size_t)(o - t);
    if (len + 1 > cap) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = t[i];
    }
    out[len] = 0;
    return len;
}
