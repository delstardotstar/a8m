#include "wav.h"

#include <string.h>

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (le16(p + 2) << 16);
}

static const uint8_t guid_tail[14] = {
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
};

static const char *parse_fmt(const uint8_t *f, uint32_t n, struct wav_info *w)
{
    uint32_t tag = le16(f), ch = le16(f + 2), rate = le32(f + 4), block = le16(f + 12),
             bits = le16(f + 14);

    if (tag == 0xfffe) {
        if (n < 40 || le16(f + 16) < 22) {
            return "BAD WAV HEADER";
        }
        if (memcmp(f + 26, guid_tail, sizeof(guid_tail))) {
            return "UNSUPPORTED FORMAT";
        }
        tag = le16(f + 24);
    }
    if (tag != 1 && !(tag == 3 && bits == 32)) {
        return "UNSUPPORTED FORMAT";
    }
    if (ch < 1 || ch > 2) {
        return "TOO MANY CHANNELS";
    }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        return "UNSUPPORTED BITS";
    }
    if (!rate || block != ch * bits / 8) {
        return "BAD WAV HEADER";
    }
    w->is_float = tag == 3;
    w->channels = (uint8_t)ch;
    w->bits = (uint8_t)bits;
    w->block = (uint8_t)block;
    w->rate = rate;
    return 0;
}

const char *wav_parse(wav_read_fn read, void *ctx, uint32_t file_size, struct wav_info *w)
{
    uint8_t h[40];
    uint32_t pos = 12, data_off = 0, data_len = 0;
    int have_fmt = 0, have_data = 0;

    memset(w, 0, sizeof(*w));
    if (read(ctx, 0, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        return "BAD WAV HEADER";
    }
    while ((!have_fmt || !have_data) && pos <= file_size - 8u && file_size >= 8u) {
        uint32_t size;

        if (read(ctx, pos, h, 8) != 8) {
            break;
        }
        size = le32(h + 4);
        if (!memcmp(h, "fmt ", 4)) {
            uint32_t n = size < sizeof(h) ? size : sizeof(h);
            const char *e;

            if (size < 16 || read(ctx, pos + 8, h, n) != n) {
                return "BAD WAV HEADER";
            }
            if ((e = parse_fmt(h, n, w))) {
                return e;
            }
            have_fmt = 1;
        } else if (!memcmp(h, "data", 4)) {
            data_off = pos + 8;
            data_len = size < file_size - data_off ? size : file_size - data_off;
            have_data = 1;
        }
        if (size > file_size - pos - 8u) {
            break;
        }
        pos += 8u + size + (size & 1u);
    }
    if (!have_fmt || !have_data) {
        return "BAD WAV HEADER";
    }
    w->frames = data_len / w->block;
    if (!w->frames) {
        return "EMPTY WAV";
    }
    if (w->frames > 0x7000000u) {   /* sample planes must fit uint32 sizes */
        return "WAV TOO LONG";
    }
    w->data_off = data_off;
    w->data_len = w->frames * w->block;
    return 0;
}

static uint8_t *put16(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
    return o + 2;
}

static uint8_t *put32(uint8_t *o, uint32_t v)
{
    return put16(put16(o, v), v >> 16);
}

static uint8_t *put_tag(uint8_t *o, const char *t)
{
    memcpy(o, t, 4);
    return o + 4;
}

uint32_t wav_header(uint8_t *h, const struct wav_info *w)
{
    uint32_t data = w->frames * w->block, fmt = w->is_float ? 18 : 16, fact = w->is_float ? 12 : 0;
    uint8_t *o = h;

    o = put_tag(o, "RIFF");
    o = put32(o, 4 + 8 + fmt + fact + 8 + data + (data & 1u));
    o = put_tag(o, "WAVE");
    o = put_tag(o, "fmt ");
    o = put32(o, fmt);
    o = put16(o, w->is_float ? 3 : 1);
    o = put16(o, w->channels);
    o = put32(o, w->rate);
    o = put32(o, w->rate * w->block);
    o = put16(o, w->block);
    o = put16(o, w->bits);
    if (w->is_float) {
        o = put16(o, 0);
        o = put_tag(o, "fact");
        o = put32(o, 4);
        o = put32(o, w->frames);
    }
    o = put_tag(o, "data");
    o = put32(o, data);
    return (uint32_t)(o - h);
}
