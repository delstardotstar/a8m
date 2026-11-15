#include "samples.h"
#include "ff.h"
#include "mmc.h"
#include "mmu.h"
#include "systime.h"
#include "wav.h"

#include <string.h>

/* Big enough that a read is one long multi-block transfer, small enough
 * (~6 ms at 10 MB/s) to keep the main loop responsive. */
#define CHUNK           (64u << 10)
#define HASH_SIZE       (2u * SAMPLES_MAX)

extern uint8_t __sample_pool[];
#define POOL (__sample_pool + POOL_GUARD)

/* The audio IRQ reads entries that are READY (samples_valid). An entry
 * becomes READY only once every field the voice uses is in place, and
 * leaves READY before any of them changes. */
#define PUBLISH() __asm__ volatile("dmb" ::: "memory")

struct sample samples[SAMPLES_MAX + SAMPLE_PREVIEWS];
static int count;
static uint16_t hash[HASH_SIZE];       /* index + 1, 0 = free */
/* The folder entries by name; unsorted while `bulk` (a folder scan). */
static uint16_t order[SAMPLES_MAX], order_tmp[SAMPLES_MAX];
static int norder, bulk;
/* Where each folder file's directory entry is (f_open_at), NOPOS if not known. */
#define NOPOS 0xffffffffu
static uint32_t dpos[SAMPLES_MAX];
/* fdate << 16 | ftime from the scan, 0 if not known. */
static uint32_t stamp[SAMPLES_MAX];
static uint32_t seen[SAMPLES_MAX / 32];
static int rescan;
static DIR fdir;
static int fdir_ok;
static uint32_t pool_used;
static int take_open;
static char dir[FF_LFN_BUF + 4];
static int stalled, nomem, bad;
static const char *last_error;
static uint32_t read_bytes, read_us;

/* The sample being loaded: its open file and the read position. */
static FIL fil;
static int open_idx = -1;
static uint32_t fpos, carry_n;
static uint8_t carry[8];
static uint8_t stage[64 + CHUNK] __attribute__((aligned(CACHE_LINE)));

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = c & 1u ? 0xedb88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[i] = c;
    }
}

uint32_t samples_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    while (n--) {
        crc = crc_table[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;

    while (*s) {
        h = (h ^ (uint32_t)lower((unsigned char)*s++)) * 16777619u;
    }
    return h;
}

static int name_eq(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

/* Stock's order: ASCII with A-Z folded to lower case ('_' before letters). */
int samples_name_cmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static int before(int i, int j)
{
    int c = samples_name_cmp(samples[i].name, samples[j].name);

    return c < 0 || (c == 0 && i < j);
}

static void order_insert(int i)
{
    int lo = 0, hi = norder;

    while (lo < hi) {
        int mid = (lo + hi) / 2;

        if (before(order[mid], i)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    memmove(&order[lo + 1], &order[lo], (size_t)(norder - lo) * sizeof(order[0]));
    order[lo] = (uint16_t)i;
    norder++;
}

static void order_remove(int i)
{
    for (int k = 0; k < norder; k++) {
        if (order[k] == i) {
            memmove(&order[k], &order[k + 1], (size_t)(norder - k - 1) * sizeof(order[0]));
            norder--;
            return;
        }
    }
}

static void close_open(void);

static void drop(int i, int state)
{
    struct sample *s = &samples[i];

    if (open_idx == i) {
        close_open();
    }
    if ((s->state == SMP_READY || s->state == SMP_LOADING) && s->pool_off + samples_need(i) == pool_used) {
        pool_used = s->pool_off;
    }
    s->state = (uint8_t)state;
    PUBLISH();
    s->plane[0] = s->plane[1] = 0;
    s->loaded = 0;
}

void samples_sort(void)
{
    uint16_t *a = order, *b = order_tmp, *t;

    if (rescan) {
        rescan = 0;
        for (int i = 0; i < count; i++) {
            if (!(seen[i / 32] & 1u << i % 32) && samples[i].name[0] && samples[i].state != SMP_MISSING &&
                !samples[i].ram_only) {
                drop(i, SMP_MISSING);
                dpos[i] = NOPOS;
            }
        }
    }

    for (int w = 1; w < norder; w *= 2) {
        for (int lo = 0; lo < norder; lo += 2 * w) {
            int m = lo + w < norder ? lo + w : norder, hi = lo + 2 * w < norder ? lo + 2 * w : norder;
            int i = lo, j = m, k = lo;

            while (i < m && j < hi) {
                b[k++] = before(a[j], a[i]) ? a[j++] : a[i++];
            }
            while (i < m) {
                b[k++] = a[i++];
            }
            while (j < hi) {
                b[k++] = a[j++];
            }
        }
        t = a;
        a = b;
        b = t;
    }
    if (a != order) {
        memcpy(order, a, (size_t)norder * sizeof(order[0]));
    }
    bulk = 0;
}

int samples_sorted(int k)
{
    return order[k];
}

int samples_sorted_count(void)
{
    return norder;
}

static int same_str(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int find(const char *name, uint32_t *slot)
{
    uint32_t h = name_hash(name) & (HASH_SIZE - 1u);

    while (hash[h]) {
        if (name_eq(samples[hash[h] - 1].name, name)) {
            *slot = h;
            return hash[h] - 1;
        }
        h = (h + 1u) & (HASH_SIZE - 1u);
    }
    *slot = h;
    return -1;
}

#define PREVIEW_PATH    320
static char preview_path[SAMPLE_PREVIEWS][PREVIEW_PATH];
static DIR preview_dir[SAMPLE_PREVIEWS];
static uint32_t preview_pos[SAMPLE_PREVIEWS];

static int add(const char *name, int state, uint32_t size)
{
    size_t n = strlen(name);
    uint32_t h;
    int i = find(name, &h);
    struct sample *s;

    if (i >= 0) {
        return i;
    }
    if (n > SAMPLE_NAME_LEN || count >= SAMPLES_MAX) {
        return -1;
    }
    s = &samples[count];
    memset(s, 0, sizeof(*s));
    memcpy(s->name, name, n + 1);
    s->state = (uint8_t)state;
    s->file_size = size;
    hash[h] = (uint16_t)(count + 1);
    dpos[count] = NOPOS;
    stamp[count] = 0;
    if (bulk) {
        order[norder++] = (uint16_t)count;
    } else {
        order_insert(count);
    }
    PUBLISH();
    return count++;
}

static void close_open(void)
{
    if (open_idx >= 0) {
        f_close(&fil);
        open_idx = -1;
    }
}

void samples_reset(const char *folder, const void *d)
{
    size_t n = strlen(folder);

    if (!crc_table[1]) {
        crc_init();
    }
    close_open();
    count = 0;
    /* Previews outlive a folder, their memory doesn't: load them again. */
    for (int i = SAMPLES_MAX; i < SAMPLES_MAX + SAMPLE_PREVIEWS; i++) {
        struct sample *s = &samples[i];
        if (s->state == SMP_READY || s->state == SMP_LOADING) {
            s->state = SMP_HEADER;
            PUBLISH();
            s->plane[0] = s->plane[1] = 0;
            s->loaded = 0;
        }
    }
    PUBLISH();
    memset(hash, 0, sizeof(hash));
    norder = 0;
    bulk = 1;
    rescan = 0;
    fdir_ok = d != 0;
    if (d) {
        fdir = *(const DIR *)d;
    }
    pool_used = 0;
    stalled = nomem = bad = 0;
    last_error = 0;
    read_bytes = read_us = 0;
    if (n > sizeof(dir) - 4) {
        n = sizeof(dir) - 4;
    }
    memcpy(dir, "1:", 2);
    memcpy(dir + 2, folder, n);
    dir[n + 2] = '/';
    dir[n + 3] = 0;
}

int samples_add(const char *name, uint32_t size)
{
    return add(name, SMP_FOUND, size);
}

void samples_rescan(const void *d)
{
    close_open();
    for (int i = 0; i < SAMPLES_MAX + SAMPLE_PREVIEWS; i++) {
        if (samples_valid(i) && samples[i].state == SMP_LOADING) {
            drop(i, SMP_HEADER);
        }
    }
    memset(seen, 0, sizeof(seen));
    rescan = 1;
    bulk = 1;
    fdir = *(const DIR *)d;
    fdir_ok = 1;
    stalled = nomem = bad = 0;
    last_error = 0;
    read_bytes = read_us = 0;
}

int samples_add_at(const char *name, uint32_t size, uint32_t pos, uint32_t st)
{
    uint32_t h;
    int i = rescan ? find(name, &h) : -1;
    struct sample *s;

    if (i < 0) {
        i = add(name, SMP_FOUND, size);
    } else {
        s = &samples[i];
        if (s->state == SMP_MISSING || s->file_size != size || (stamp[i] && stamp[i] != st)) {
            drop(i, SMP_FOUND);
            s->file_size = size;
            s->err = 0;
        } else if (s->state == SMP_NOMEM) {
            s->state = SMP_HEADER;
        } else if (s->state == SMP_BAD) {
            s->state = SMP_FOUND;
            s->err = 0;
        }
        memcpy(s->name, name, strlen(name) + 1);
    }
    if (i >= 0) {
        dpos[i] = pos;
        stamp[i] = st;
        seen[i / 32] |= 1u << i % 32;
    }
    return i;
}

int samples_ref(const char *name)
{
    return add(name, SMP_MISSING, 0);
}

int samples_count(void)
{
    return count;
}

static int open_file(FIL *f, const struct sample *s)
{
    char p[sizeof(dir) + SAMPLE_NAME_LEN];
    size_t d = strlen(dir), i = (size_t)(s - samples);

    if (s->path) {
        if (i >= SAMPLES_MAX && preview_pos[i - SAMPLES_MAX] != NOPOS &&
            f_open_at(f, &preview_dir[i - SAMPLES_MAX], preview_pos[i - SAMPLES_MAX], s->name) == FR_OK) {
            return 1;
        }
        return f_open(f, s->path, FA_READ) == FR_OK;
    }
    if (fdir_ok && dpos[i] != NOPOS && f_open_at(f, &fdir, dpos[i], s->name) == FR_OK) {
        return 1;
    }
    memcpy(p, dir, d);
    memcpy(p + d, s->name, strlen(s->name) + 1);
    return f_open(f, p, FA_READ) == FR_OK;
}

static uint32_t fil_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    UINT got = 0;

    if (f_lseek((FIL *)ctx, off) != FR_OK || f_read((FIL *)ctx, buf, n, &got) != FR_OK) {
        return 0;
    }
    return got;
}

static void set_bad(struct sample *s, const char *why)
{
    s->state = SMP_BAD;
    s->err = why;
    last_error = why;
    bad++;
}

static void read_header(struct sample *s)
{
    struct wav_info w;
    FIL f;
    const char *e;

    if (!open_file(&f, s)) {
        set_bad(s, "CAN'T OPEN WAV");
        return;
    }
    e = wav_parse(fil_read, &f, (uint32_t)f_size(&f), &w);
    f_close(&f);
    if (e) {
        set_bad(s, e);
        return;
    }
    s->state = SMP_HEADER;
    s->channels = w.channels;
    s->bits = w.bits;
    s->block = w.block;
    s->is_float = w.is_float;
    s->rate = w.rate;
    s->frames = w.frames;
    s->data_off = w.data_off;
}

static uint32_t plane_bytes(const struct sample *s)
{
    return ((s->frames + 2u * SAMPLE_GUARD) * 4u + CACHE_LINE - 1u) & ~(CACHE_LINE - 1u);
}

uint32_t samples_need(int i)
{
    return plane_bytes(&samples[i]) * samples[i].channels;
}

uint32_t samples_pool_size(void)
{
    uint32_t size = DMA_REGION - POOL_GUARD - (uint32_t)POOL;
#ifdef SAMPLE_POOL_LIMIT
    size = size < SAMPLE_POOL_LIMIT ? size : SAMPLE_POOL_LIMIT;
#endif
    return size;
}

const uint8_t *samples_pool_lo(void)
{
    return POOL;
}

const uint8_t *samples_pool_hi(void)
{
    return POOL + samples_pool_size();
}

uint32_t samples_pool_used(void)
{
    return pool_used;
}

static int start(int i)
{
    struct sample *s = &samples[i];
    uint32_t pb = plane_bytes(s), need = samples_need(i);

    if (need > samples_pool_size() - pool_used) {
        s->state = SMP_NOMEM;
        s->err = "NOT ENOUGH MEMORY";
        last_error = s->err;
        nomem++;
        return 0;
    }
    for (int c = 0; c < s->channels; c++) {
        int32_t *p = (int32_t *)(POOL + pool_used + pb * (uint32_t)c);

        memset(p, 0, SAMPLE_GUARD * 4u);
        memset(p + SAMPLE_GUARD + s->frames, 0, SAMPLE_GUARD * 4u);
        s->plane[c] = p + SAMPLE_GUARD;
    }
    if (s->channels == 1) {
        s->plane[1] = s->plane[0];
    }
    if (!open_file(&fil, s)) {
        set_bad(s, "CAN'T OPEN WAV");
        return 0;
    }
    s->pool_off = pool_used;
    pool_used += need;
    open_idx = i;
    fpos = s->data_off & ~511u;
    carry_n = 0;
    s->loaded = 0;
    s->crc = 0;
    s->state = SMP_LOADING;
    return 1;
}

static int32_t from_float(const uint8_t *p)
{
    float x;

    memcpy(&x, p, 4);
    if (x >= 1.0f) {
        return INT32_MAX;
    }
    if (x > -1.0f) {
        return (int32_t)(x * 2147483648.0f);
    }
    return x == x ? INT32_MIN : 0;
}

static void convert(struct sample *s, const uint8_t *p, uint32_t n)
{
    int32_t *l = s->plane[0] + s->loaded, *r = s->plane[1] + s->loaded;

    switch (s->bits * 2 + s->channels - 1 + (s->is_float ? 100 : 0)) {
    case 16:
        for (uint32_t i = 0; i < n; i++) {
            l[i] = (int32_t)((uint32_t)p[i] << 24) ^ INT32_MIN;
        }
        break;
    case 17:
        for (uint32_t i = 0; i < n; i++, p += 2) {
            l[i] = (int32_t)((uint32_t)p[0] << 24) ^ INT32_MIN;
            r[i] = (int32_t)((uint32_t)p[1] << 24) ^ INT32_MIN;
        }
        break;
    case 32:
        for (uint32_t i = 0; i < n; i++, p += 2) {
            l[i] = (int32_t)((uint32_t)p[0] << 16 | (uint32_t)p[1] << 24);
        }
        break;
    case 33:
        for (uint32_t i = 0; i < n; i++, p += 4) {
            l[i] = (int32_t)((uint32_t)p[0] << 16 | (uint32_t)p[1] << 24);
            r[i] = (int32_t)((uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        }
        break;
    case 48:
        for (uint32_t i = 0; i < n; i++, p += 3) {
            l[i] = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
        }
        break;
    case 49:
        for (uint32_t i = 0; i < n; i++, p += 6) {
            l[i] = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
            r[i] = (int32_t)((uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24);
        }
        break;
    case 64:
        memcpy(l, p, n * 4u);
        break;
    case 65:
        for (uint32_t i = 0; i < n; i++, p += 8) {
            memcpy(&l[i], p, 4);
            memcpy(&r[i], p + 4, 4);
        }
        break;
    case 164:
        for (uint32_t i = 0; i < n; i++, p += 4) {
            l[i] = from_float(p);
        }
        break;
    case 165:
        for (uint32_t i = 0; i < n; i++, p += 8) {
            l[i] = from_float(p);
            r[i] = from_float(p + 4);
        }
        break;
    }
    s->loaded += n;
}

int samples_load_step(int i)
{
    struct sample *s = &samples[i];
    uint32_t end, want, skip, got, t0, n;
    UINT br = 0;
    uint8_t *p;

    if (s->state == SMP_HEADER && open_idx != i) {
        if (take_open) {
            return 0;
        }
        close_open();
        if (!start(i)) {
            return 1;
        }
    }
    if (s->state != SMP_LOADING || open_idx != i) {
        return 1;
    }
    end = s->data_off + s->frames * s->block;
    want = end - fpos < CHUNK ? end - fpos : CHUNK;
    want = (want + 511u) & ~511u;
    t0 = time_us();
    if (f_lseek(&fil, fpos) != FR_OK || f_read(&fil, stage + 64, want, &br) != FR_OK) {
        close_open();
        set_bad(s, mmc_error ? mmc_error : "CARD READ ERROR");
        stalled = 1;
        return 1;
    }
    read_us += time_us() - t0;
    read_bytes += br;
    skip = fpos < s->data_off ? s->data_off - fpos : 0;
    got = br;
    if (fpos + got > end) {
        got = end - fpos;
    }
    if (got <= skip) {
        close_open();
        set_bad(s, "WAV CUT SHORT");
        return 1;
    }
    p = stage + 64 + skip;
    got -= skip;
    s->crc = samples_crc32(s->crc, p, got);
    p -= carry_n;
    memcpy(p, carry, carry_n);
    got += carry_n;
    n = got / s->block;
    if (n > s->frames - s->loaded) {
        n = s->frames - s->loaded;
    }
    convert(s, p, n);
    carry_n = got - n * s->block;
    memcpy(carry, p + n * s->block, carry_n);
    fpos += br;
    if (s->loaded < s->frames) {
        return 0;
    }
    close_open();
    PUBLISH();
    s->state = SMP_READY;
    return 1;
}

uint8_t *samples_take_begin(uint32_t *bytes)
{
    take_open = 1;
    *bytes = samples_pool_size() - pool_used;
    return POOL + pool_used;
}

void samples_take_end(uint32_t used)
{
    pool_used += used;
    take_open = 0;
}

int samples_take_open(void)
{
    return take_open;
}

int samples_add_ram(const char *name, int channels, uint32_t rate, uint32_t frames, int32_t *l, int32_t *r,
                    uint32_t off)
{
    int i = add(name, SMP_MISSING, 0);
    struct sample *s;

    if (i < 0) {
        return -1;
    }
    s = &samples[i];
    s->state = SMP_MISSING;
    PUBLISH();
    s->channels = (uint8_t)channels;
    s->bits = 24;
    s->block = (uint8_t)(3 * channels);
    s->is_float = 0;
    s->ram_only = 1;
    s->rate = rate;
    s->frames = frames;
    s->data_off = 44;
    s->file_size = 44 + ((frames * s->block + 1u) & ~1u);
    s->plane[0] = l;
    s->plane[1] = r;
    s->pool_off = pool_used + off;
    s->loaded = frames;
    s->crc = 0;
    s->err = 0;
    PUBLISH();
    s->state = SMP_READY;
    return i;
}

int samples_stalled(void)
{
    return stalled;
}

uint32_t samples_read_bytes(void)
{
    return read_bytes;
}

uint32_t samples_read_us(void)
{
    return read_us;
}

int samples_nomem(void)
{
    return nomem;
}

int samples_bad(void)
{
    return bad;
}

const char *samples_last_error(void)
{
    return last_error;
}

int samples_find(const char *name)
{
    uint32_t h;

    return find(name, &h);
}

void samples_rename(int i, const char *to)
{
    uint32_t h;
    size_t n = strlen(to);

    if (n > SAMPLE_NAME_LEN) {
        n = SAMPLE_NAME_LEN;
    }
    order_remove(i);
    memcpy(samples[i].name, to, n);
    samples[i].name[n] = 0;
    dpos[i] = NOPOS;
    if (find(samples[i].name, &h) < 0) {
        hash[h] = (uint16_t)(i + 1);
    }
    order_insert(i);
}

void samples_forget(int i)
{
    struct sample *s = &samples[i];

    if (open_idx == i) {
        close_open();
    }
    s->state = SMP_MISSING;
    PUBLISH();
    s->name[0] = 0;
    order_remove(i);
}

int samples_unload(int i)
{
    struct sample *s = &samples[i];

    if ((s->state != SMP_READY && s->state != SMP_LOADING) || s->pool_off + samples_need(i) != pool_used) {
        return 0;
    }
    if (open_idx == i) {
        close_open();
    }
    s->state = SMP_HEADER;
    PUBLISH();
    pool_used = s->pool_off;
    s->loaded = 0;
    s->plane[0] = s->plane[1] = 0;
    return 1;
}

static int holds(const struct sample *s)
{
    return s->state == SMP_READY || s->state == SMP_LOADING;
}

uint32_t samples_lost(void)
{
    uint32_t held = 0;

    for (int i = 0; i < SAMPLES_MAX + SAMPLE_PREVIEWS; i++) {
        if (samples_valid(i) && samples[i].ram_only) {
            return 0;
        }
        if (samples_valid(i) && holds(&samples[i])) {
            held += samples_need(i);
        }
    }
    return held < pool_used ? pool_used - held : 0;
}

void samples_repack(void)
{
    close_open();
    for (int i = 0; i < SAMPLES_MAX + SAMPLE_PREVIEWS; i++) {
        struct sample *s = &samples[i];

        if (samples_valid(i) && (holds(s) || s->state == SMP_NOMEM)) {
            s->state = SMP_HEADER;
            PUBLISH();
            s->plane[0] = s->plane[1] = 0;
            s->loaded = 0;
        }
    }
    pool_used = 0;
    nomem = 0;
}

int samples_probe(int i)
{
    if (samples[i].state == SMP_FOUND) {
        read_header(&samples[i]);
    }
    return samples[i].state == SMP_HEADER;
}

void samples_refresh(int i, uint32_t size)
{
    struct sample *s = &samples[i];

    if (open_idx == i) {
        close_open();
    }
    samples_unload(i);
    s->state = SMP_FOUND;
    PUBLISH();
    s->plane[0] = s->plane[1] = 0;
    s->loaded = 0;
    s->file_size = size;
    dpos[i] = NOPOS;
    stamp[i] = 0;
    read_header(s);
}

int samples_preview(const char *d, const char *name)
{
    return samples_preview_at(d, name, 0, NOPOS);
}

int samples_preview_at(const char *d, const char *name, const void *dj, uint32_t pos)
{
    size_t dn = strlen(d), nn = strlen(name);
    struct sample *s;
    int slot = -1;

    if (nn > SAMPLE_NAME_LEN || dn + nn + 4 >= PREVIEW_PATH) {
        return -1;
    }
    for (int i = 0; i < SAMPLE_PREVIEWS; i++) {
        char *p = preview_path[i];
        if (!p[0]) {
            slot = slot < 0 ? i : slot;
        } else if (!memcmp(p + 2, d, dn) && p[2 + dn] == '/' && same_str(p + 3 + dn, name)) {
            return SAMPLES_MAX + i;
        }
    }
    if (slot < 0) {
        return -1;
    }
    memcpy(preview_path[slot], "1:", 2);
    memcpy(preview_path[slot] + 2, d, dn);
    preview_path[slot][2 + dn] = '/';
    memcpy(preview_path[slot] + 3 + dn, name, nn + 1);
    preview_pos[slot] = dj ? pos : NOPOS;
    if (dj) {
        preview_dir[slot] = *(const DIR *)dj;
    }
    s = &samples[SAMPLES_MAX + slot];
    memset(s, 0, sizeof(*s));
    memcpy(s->name, name, nn + 1);
    s->path = preview_path[slot];
    s->state = SMP_FOUND;
    read_header(s);
    if (s->state == SMP_BAD) {
        bad--;
    } else if (s->frames > s->rate * SAMPLE_PREVIEW_SECS) {
        s->frames = s->rate * SAMPLE_PREVIEW_SECS;
    }
    return SAMPLES_MAX + slot;
}

void samples_preview_release(int i)
{
    struct sample *s = &samples[i];

    if (i < SAMPLES_MAX || !s->path) {
        return;
    }
    samples_unload(i);
    s->state = SMP_MISSING;
    ((char *)s->path)[0] = 0;
    s->path = 0;
    s->name[0] = 0;
}

int samples_regen(int i, uint32_t frame, uint32_t n, uint8_t *o)
{
    const struct sample *s = &samples[i];

    if (s->state != SMP_READY || s->is_float) {
        return 0;
    }
    for (uint32_t f = frame; f < frame + n; f++) {
        for (int c = 0; c < s->channels; c++) {
            uint32_t v = (uint32_t)s->plane[c][f];

            switch (s->bits) {
            case 8:
                *o++ = (uint8_t)((v >> 24) ^ 0x80u);
                break;
            case 16:
                *o++ = (uint8_t)(v >> 16);
                *o++ = (uint8_t)(v >> 24);
                break;
            case 24:
                *o++ = (uint8_t)(v >> 8);
                *o++ = (uint8_t)(v >> 16);
                *o++ = (uint8_t)(v >> 24);
                break;
            default:
                memcpy(o, &v, 4);
                o += 4;
            }
        }
    }
    return 1;
}

int samples_preview_next(void)
{
    int best = -1;

    for (int i = 0; i < SAMPLE_PREVIEWS; i++) {
        const struct sample *s = &samples[SAMPLES_MAX + i];

        if (s->path && (best < 0 || s->pool_off > samples[best].pool_off)) {
            best = SAMPLES_MAX + i;
        }
    }
    return best;
}
