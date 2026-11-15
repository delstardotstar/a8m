#ifndef WAV_H_INCLUDED
#define WAV_H_INCLUDED

#include <stdint.h>

/*
 * RIFF/WAVE headers. Freestanding (host-tested by tools/ymltest.sh).
 * Accepted (as stock v3.04 in the emulator): integer PCM 8/16/24/32-bit
 * and 32-bit float, mono or stereo, any rate, plain or
 * WAVE_FORMAT_EXTENSIBLE. Chunks are walked in
 * order (odd sizes padded); unknown ones are skipped. A data chunk running
 * past the end of the file is clipped to it.
 */

struct wav_info {
    uint32_t rate;
    uint32_t frames;
    uint32_t data_off;      /* file offset of the first frame */
    uint32_t data_len;      /* bytes, = frames * block */
    uint8_t channels;
    uint8_t bits;           /* container bits per sample: 8, 16, 24, 32 */
    uint8_t block;          /* bytes per frame */
    uint8_t is_float;       /* IEEE float (32-bit) */
};

/* Reads n bytes at off into buf; returns the count read (short at EOF). */
typedef uint32_t (*wav_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t n);

/* 0, or why the file can't be used (short upper-case text). */
const char *wav_parse(wav_read_fn read, void *ctx, uint32_t file_size, struct wav_info *w);

/* Writes the header of a new file holding w->frames frames in w's format
 * (rate, channels, bits, is_float) into h (WAV_HEADER_MAX bytes): RIFF,
 * fmt, (float: fact,) and the data chunk's header. Returns its length; the
 * frames follow, then one zero byte if the data length is odd. No other
 * chunks. */
#define WAV_HEADER_MAX 64
uint32_t wav_header(uint8_t *h, const struct wav_info *w);

#endif
