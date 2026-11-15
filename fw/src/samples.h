#ifndef SAMPLES_H_INCLUDED
#define SAMPLES_H_INCLUDED

#include <stdint.h>

/*
 * The loaded folder's WAVs and the sample memory pool (all DDR between our
 * image's arenas and the uncached DMA region). Each distinct file (names
 * compare case-insensitively) is one entry and is loaded at most once,
 * however many zones use it. The pool is a bump allocator, emptied when a
 * folder loads. Main loop only; the audio side may read READY entries.
 *
 * In-RAM format, for the voice: per channel a plane of int32 Q31
 * (left-justified: 8-bit unsigned -> (x - 128) << 24, 16-bit << 16, 24-bit
 * << 8, 32-bit as is: lossless; float x -> x * 2^31 truncated, clipped
 * to +-1.0), `frames` long, with SAMPLE_GUARD zero
 * frames before index 0 and after the last frame so the interpolation filter
 * never needs a bounds check. Planes start 16-byte aligned. A mono sample
 * has plane[1] == plane[0]. Rate is the file's.
 */

#define SAMPLES_MAX     16384   /* zone_cfg.smp and the order index are uint16 */
#define SAMPLE_NAME_LEN 47
#define SAMPLE_GUARD    64      /* half the longest interpolation filter */

enum {
    SMP_FOUND,      /* in the folder, header not read */
    SMP_HEADER,     /* header read, not loaded */
    SMP_LOADING,
    SMP_READY,
    SMP_NOMEM,      /* didn't fit in the pool */
    SMP_BAD,        /* unusable file (err says why) */
    SMP_MISSING     /* named by a preset, not in the folder */
};

struct sample {
    char name[SAMPLE_NAME_LEN + 1];
    uint8_t state;
    uint8_t channels;
    uint8_t bits;
    uint8_t block;
    uint8_t is_float;
    uint8_t ram_only;       /* a recording whose file isn't written (yet) */
    uint32_t rate;
    uint32_t frames;
    uint32_t file_size;
    uint32_t data_off;
    int32_t *plane[2];
    const char *path;       /* previews: the file's full path ("1:..."), else 0 */
    uint32_t pool_off;      /* where its planes start in the pool */
    uint32_t loaded;        /* frames converted so far */
    uint32_t crc;           /* CRC-32 (zlib) of the data chunk's bytes as read */
    const char *err;
};

int samples_count(void);
#define SAMPLE_PREVIEWS 16
extern struct sample samples[SAMPLES_MAX + SAMPLE_PREVIEWS];
/* Whether samples[i] is an entry (a folder's or a preview). */
static inline int samples_valid(int i)
{
    return (i >= 0 && i < samples_count()) || (i >= SAMPLES_MAX && i < SAMPLES_MAX + SAMPLE_PREVIEWS);
}

/* Forgets the samples and empties the pool; files are then looked up in
 * 1:<folder>/. d: the folder's DIR as f_opendir left it (for samples_add_at),
 * or 0 (void: ui.c can't include ff.h, its names clash).
 * Until samples_sort, entries are added unsorted. */
void samples_reset(const char *folder, const void *d);
/* A WAV found in the folder; -1 if the table is full or the name too long. */
int samples_add(const char *name, uint32_t size);
/* Same, from a folder scan: pos is the DIR's dptr before the f_readdir that
 * returned it, so opening it needs no directory search; stamp is the
 * file's fdate << 16 | ftime. */
int samples_add_at(const char *name, uint32_t size, uint32_t pos, uint32_t stamp);
/* Instead of samples_reset when the same folder is scanned again: entries
 * keep their index and, if their file is unchanged (name, size, stamp),
 * their memory. Through samples_sort, samples_add_at refreshes an entry or
 * adds a new one; files that changed load again when needed (the old
 * memory is lost unless it was the last allocation). */
void samples_rescan(const void *d);
/* Sorts the entries added since samples_reset by name; later ones are kept
 * in order as they come. After samples_rescan, files no longer in the
 * folder become MISSING. */
void samples_sort(void);
/* The k-th named entry by name (case-insensitively), k < samples_sorted_count(). */
int samples_sorted(int k);
int samples_sorted_count(void);
int samples_name_cmp(const char *a, const char *b);
/* The entry named so (added as SMP_MISSING if the folder has no such
 * file); -1 if the table is full or the name too long. */
int samples_ref(const char *name);
int samples_count(void);

/* Loads one chunk of sample i (allocating it first); 1 once it is done
 * (READY, NOMEM or BAD). A card error also stops the loader (stalled). */
int samples_load_step(int i);
int samples_stalled(void);
/* Bytes of sample memory a header-read sample will take. */
uint32_t samples_need(int i);

/* The entry named so, -1 if none. */
int samples_find(const char *name);
/* Renames an entry (the file is renamed by the caller). */
void samples_rename(int i, const char *to);
/* An erased file: the entry stays (presets index by position) but is
 * nameless and unusable. */
void samples_forget(int i);
/* Frees a loaded or loading sample's memory again if it is the most recent
 * allocation; 1 if freed (it then loads again when needed). */
int samples_unload(int i);
/* Reads the header of an entry added but not yet read; 1 if usable. */
int samples_probe(int i);
/* The file was rewritten (new size): its header is read again and its
 * memory dropped, which is reused when it was the latest allocation. It
 * loads again when needed. Callers stop the voices first. */
void samples_refresh(int i, uint32_t size);
/* Previews of files outside the folder: entries from SAMPLES_MAX up with
 * their own path, header read at once, loading like any other (at most
 * SAMPLE_PREVIEW_SECS of it). -1 if it can't be used. Released, the
 * memory goes back to the pool when it was the last allocation. */
#define SAMPLE_PREVIEW_SECS 20
int samples_preview(const char *dir, const char *name);
/* The same, opened by its directory position (f_open_at: d is a DIR copy as
 * f_opendir left it, pos the dptr before the f_readdir that returned it),
 * which skips FatFs's linear name search in a big directory. */
int samples_preview_at(const char *dir, const char *name, const void *d, uint32_t pos);
void samples_preview_release(int i);
/* The live preview that was allocated last, -1 if none. */
int samples_preview_next(void);

/* The data-chunk bytes of n frames from `frame` as the file had them (integer
 * formats only; 0 for others). */
int samples_regen(int i, uint32_t frame, uint32_t n, uint8_t *o);

/* Recording: from samples_take_begin to samples_take_end all free sample
 * memory (bytes, from the returned address, 64-byte aligned) belongs to the
 * take and the loader starts nothing; the end keeps `used` bytes of it. */
uint8_t *samples_take_begin(uint32_t *bytes);
void samples_take_end(uint32_t used);
int samples_take_open(void);
/* A new folder entry for a recording already in sample memory (24-bit
 * PCM, READY), its planes at pool offset off from the take's start. -1 if
 * the table is full. */
int samples_add_ram(const char *name, int channels, uint32_t rate, uint32_t frames, int32_t *l, int32_t *r,
                    uint32_t off);
uint32_t samples_crc32(uint32_t crc, const uint8_t *p, uint32_t n);

/* Pool bytes no entry holds (files changed or removed since they loaded). */
uint32_t samples_lost(void);
/* Drops all sample memory, entries kept (they load again when needed).
 * Callers stop the voices first. */
void samples_repack(void);

uint32_t samples_pool_used(void);
uint32_t samples_pool_size(void);
/* The pool's address range. */
const uint8_t *samples_pool_lo(void);
const uint8_t *samples_pool_hi(void);
/* Card read throughput of the loads so far: bytes and time inside f_read. */
uint32_t samples_read_bytes(void);
uint32_t samples_read_us(void);
/* Samples not loaded for lack of memory / unusable files, and the last
 * problem (for a message), since the folder loaded. */
int samples_nomem(void);
int samples_bad(void);
const char *samples_last_error(void);

#endif
