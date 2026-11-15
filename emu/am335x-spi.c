/*
 * AM335x McSPI master (TRM chapter 24), 4 channels. Each TX word completes
 * instantly. Channel 0 has a MAX11128 ADC attached (the Assimil8or's CV
 * ADCs). PIO words are logged.
 *
 * DMA: while channel 0 is enabled with DMAW/DMAR set, a timer streams one
 * ADC sequence (TX event, then RX event, per word) per scan period of device
 * time (48 kHz, or $A8_CV_SCAN_HZ), each word sampled 1/16 of a period after
 * the previous one.
 *
 * Results carry the channel number in bits 15:12 when the mode word set
 * CHAN_ID, else the code is shifted up 3. MAX11128 inputs are QOM properties ch0..ch15 (raw 12-bit codes), e.g.
 * "qom-set /machine/mcspi0 ch3 3000" in the monitor.
 *
 * Test signals: "qom-set /machine/mcspi0 wave 'CH SHAPE FREQ AMP OFF'"
 * replaces input CH by a generator evaluated in device time (the virtual
 * clock divided by the time scale), phase 0 at the command. Codes: OFF +
 * AMP * shape, clamped to 0..4095; SHAPE = sin, tri, sq, saw, or slope
 * (OFF + AMP codes per second); "CH off" goes back to chCH. With
 * $A8_AUDIO_DIR set, cv.raw gets, per McASP0 frame, the 12 inputs of each
 * ADC as u16 (mcspi0 ch0-11, mcspi1 ch0-11), in step with mcasp0.raw.
 * "qom-set /machine/mcspi1 fault 'POS WORD'" replaces the result at sequence
 * position POS (0-15) by WORD, as a bad word on hardware; "off" ends it.
 * Channel outputs patched with McASP0's "loop" drive inputs 0-3 (CV A).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qemu/timer.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include <math.h>
#include "am335x.h"

#define NCH 4
#define CH(off) (((off) - 0x12c) / 0x14)
#define CHREG(off) (((off) - 0x12c) % 0x14)

#define STAT_RXS 1
#define STAT_TXS 2
#define STAT_EOT 4

OBJECT_DECLARE_SIMPLE_TYPE(A8SpiState, A8_SPI)

struct A8SpiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    char *name;
    uint32_t sysconfig;
    uint32_t irqstatus;
    uint32_t irqenable;
    uint32_t wakeup;
    uint32_t syst;
    uint32_t modulctrl;
    uint32_t xferlevel;
    uint32_t conf[NCH];
    uint32_t stat[NCH];
    uint32_t ctrl[NCH];
    uint32_t rx[NCH];
    DeviceState *edma;
    int32_t tx_event;
    int32_t rx_event;
    QEMUTimer *timer;
    /* MAX11128 */
    uint16_t adc_in[16];
    uint8_t seq[16];
    int seq_len;
    int seq_fill;
    int seq_pos;
    bool chan_id;
    struct { int shape; double freq, amp, off, t0; } wave[16];
    double word_t;
    int64_t next_ns;
    char *wave_str;
    int fault_pos;
    uint16_t fault_word;
};

enum { W_OFF, W_SIN, W_TRI, W_SQ, W_SAW, W_SLOPE };
/* Scans per second of device time: $A8_CV_SCAN_HZ, default 48000 (the
 * hardware's own rate is set by SPI and DMA timing, not the audio clock). */
static double scan_hz = 48000.0;
#define SCAN_NS (NANOSECONDS_PER_SECOND / scan_hz)

static A8SpiState *spis[2];
static FILE *tracef;

/* Seconds of device time: the virtual clock, slowed by the time scale. */
static double a8_dev_s(void)
{
    static double t;
    static int64_t last;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    t += (double)(now - last) / a8_timescale / NANOSECONDS_PER_SECOND;
    last = now;
    return t;
}

/* A looped channel output on input ch (1A-4A on ADC 0, 5A-8A on ADC 1):
 * ~374.5 codes per volt, inverted, with 1 code of triangular dither as the
 * hardware's noise gives. */
static bool a8_spi_loop(A8SpiState *s, int ch, uint16_t *code)
{
    static uint32_t seed = 1;
    double v, d;

    if (ch > 3 || !a8_mcasp_loop_cv((s == spis[1]) * 4 + ch, &v)) {
        return false;
    }
    seed = seed * 1664525u + 1013904223u;
    d = (seed >> 8) / 16777216.0;
    seed = seed * 1664525u + 1013904223u;
    d -= (seed >> 8) / 16777216.0;
    *code = (uint16_t)MIN(MAX(lround(2048.0 - v * 374.5 + d), 0), 4095);
    return true;
}

static uint16_t a8_spi_input(A8SpiState *s, int ch, double t)
{
    double x, ph, v;
    uint16_t code;

    if (a8_spi_loop(s, ch, &code)) {
        return code;
    }
    if (!s->wave[ch].shape) {
        return s->adc_in[ch];
    }
    t -= s->wave[ch].t0;
    ph = t * s->wave[ch].freq;
    ph -= floor(ph);
    switch (s->wave[ch].shape) {
    case W_SIN: x = sin(2 * M_PI * ph); break;
    case W_TRI: x = ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph; break;
    case W_SQ: x = ph < 0.5 ? 1 : -1; break;
    case W_SAW: x = 2 * ph - 1; break;
    default: x = t; break;
    }
    v = s->wave[ch].off + s->wave[ch].amp * x;
    return (uint16_t)MIN(MAX(lround(v), 0), 4095);
}

void a8_spi_trace(void)
{
    uint16_t w[24];
    double t;

    if (!tracef || !spis[0] || !spis[1]) {
        return;
    }
    t = a8_dev_s();
    for (int i = 0; i < 24; i++) {
        w[i] = a8_spi_input(spis[i / 12], i % 12, t);
    }
    fwrite(w, 2, 24, tracef);
}

/* MAX11128 register writes (bit 15 set) and SampleSet sequence loading. */
static uint32_t a8_max11128(A8SpiState *s, uint32_t tx, int wl)
{
    if (wl == 8) {
        if (s->seq_fill < s->seq_len) {
            s->seq[s->seq_fill++] = tx >> 4;
        }
        if (s->seq_fill < s->seq_len) {
            s->seq[s->seq_fill++] = tx & 15;
        }
        return 0;
    }
    if (tx & 0x8000) {
        if (((tx >> 11) & 0x1f) == 0x16) {
            s->seq_len = MIN(((tx >> 3) & 0xff) + 1, 16);
            s->seq_fill = 0;
            s->seq_pos = 0;
        }
        return 0;
    }
    if (((tx >> 5) & 3) == 2) {
        s->seq_len = 0;
    }
    if ((tx >> 11) & 0xf) {
        s->chan_id = (tx >> 2) & 1;     /* ADC mode control: CHAN_ID */
    }
    if (!s->seq_len) {
        return 0;
    }
    tx = a8_spi_input(s, s->seq[s->seq_pos], s->word_t) & 0xfff;
    if (s->chan_id) {
        tx |= (uint32_t)s->seq[s->seq_pos] << 12;
    } else {
        /* Without channel IDs the 12-bit result sits in bits 14..3: stock's
         * calibration defaults (offset -16384) only zero 0 V that way. */
        tx <<= 3;
    }
    if (s->seq_pos == s->fault_pos) {
        tx = s->fault_word;
    }
    s->seq_pos = (s->seq_pos + 1) % s->seq_len;
    return tx;
}

static uint32_t a8_spi_respond(A8SpiState *s, int ch, uint32_t tx)
{
    int wl = ((s->conf[ch] >> 7) & 0x1f) + 1;

    return ch == 0 ? a8_max11128(s, tx, wl) : 0;
}

static bool a8_spi_dma_on(A8SpiState *s)
{
    return (s->ctrl[0] & 1) && (s->conf[0] & (3u << 14)) && s->edma &&
           s->tx_event >= 0;
}

/* One scan per period; a late timer catches up (up to MAX_CATCHUP scans)
 * rather than dropping scans, as the hardware never does. */
#define MAX_CATCHUP 64

static void a8_spi_stream(void *opaque)
{
    A8SpiState *s = opaque;
    int n = s->seq_len ? s->seq_len : 16;
    double t = a8_dev_s();
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int k = 0; k < MAX_CATCHUP; k++) {
        if (!a8_spi_dma_on(s)) {
            return;
        }
        for (int i = 0; i < n; i++) {
            s->word_t = t + i / (16 * scan_hz);
            a8_edma_event_now(s->edma, s->tx_event);
            if (s->conf[0] & (1u << 15)) {
                a8_edma_event_now(s->edma, s->rx_event);
            }
        }
        s->next_ns += SCAN_NS * a8_timescale;
        if (s->next_ns > now) {
            break;
        }
    }
    if (s->next_ns < now) {
        s->next_ns = now;
    }
    timer_mod(s->timer, s->next_ns);
}

static void a8_spi_update(A8SpiState *s)
{
    qemu_set_irq(s->irq, !!(s->irqstatus & s->irqenable));
}

static void a8_spi_reset(A8SpiState *s)
{
    s->sysconfig = 0;
    s->irqstatus = 0;
    s->irqenable = 0;
    s->modulctrl = 4;
    s->xferlevel = 0;
    for (int i = 0; i < NCH; i++) {
        s->conf[i] = 0x00060000;
        s->stat[i] = STAT_TXS | STAT_EOT;
        s->ctrl[i] = 0;
        s->rx[i] = 0;
    }
}

static uint64_t a8_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    A8SpiState *s = opaque;

    if (offset >= 0x12c && offset < 0x12c + NCH * 0x14) {
        int ch = CH(offset);
        switch (CHREG(offset)) {
        case 0x00: return s->conf[ch];
        case 0x04: return s->stat[ch];
        case 0x08: return s->ctrl[ch];
        case 0x0c: return 0;
        case 0x10:
            s->stat[ch] &= ~STAT_RXS;
            s->irqstatus &= ~(4u << (ch * 4));
            a8_spi_update(s);
            return s->rx[ch];
        }
    }
    switch (offset) {
    case 0x000: return 0x40300a0b;
    case 0x110: return s->sysconfig;
    case 0x114: return 1;
    case 0x118: return s->irqstatus;
    case 0x11c: return s->irqenable;
    case 0x120: return s->wakeup;
    case 0x124: return s->syst;
    case 0x128: return s->modulctrl;
    case 0x17c: return s->xferlevel;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read 0x%03" HWADDR_PRIx "\n",
                  s->name, offset);
    return 0;
}

static void a8_spi_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    A8SpiState *s = opaque;
    uint32_t v = value;

    if (offset >= 0x12c && offset < 0x12c + NCH * 0x14) {
        int ch = CH(offset);
        switch (CHREG(offset)) {
        case 0x00:
            if ((v ^ s->conf[ch]) & (1u << 20)) {
                qemu_log_mask(LOG_UNIMP, "%s.%d: cs %s\n", s->name, ch,
                              v & (1u << 20) ? "assert" : "release");
            }
            if (v != s->conf[ch] && (v & ~(1u << 20)) !=
                (s->conf[ch] & ~(1u << 20))) {
                qemu_log_mask(LOG_UNIMP, "%s.%d: conf 0x%08x\n",
                              s->name, ch, v);
            }
            s->conf[ch] = v;
            return;
        case 0x08:
            s->ctrl[ch] = v;
            if (v & 1) {
                s->irqstatus |= 1u << (ch * 4);
                a8_spi_update(s);
            }
            if (ch == 0 && a8_spi_dma_on(s) && !timer_pending(s->timer)) {
                s->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCAN_NS * a8_timescale;
                timer_mod(s->timer, s->next_ns);
            }
            return;
        case 0x0c: {
            uint32_t rx;
            if (!(s->conf[ch] & (3u << 14))) {
                s->word_t = a8_dev_s();
            }
            rx = a8_spi_respond(s, ch, v);
            if (!(s->conf[ch] & (3u << 14))) {
                qemu_log_mask(LOG_UNIMP, "%s.%d: tx 0x%04x rx 0x%04x\n",
                              s->name, ch, v, rx);
            }
            s->rx[ch] = rx;
            s->stat[ch] |= STAT_RXS | STAT_TXS | STAT_EOT;
            s->irqstatus |= (1u | 4u) << (ch * 4);
            a8_spi_update(s);
            return;
        }
        }
        return;
    }
    switch (offset) {
    case 0x110:
        if (v & 2) {
            a8_spi_reset(s);
        } else {
            s->sysconfig = v;
        }
        return;
    case 0x118: s->irqstatus &= ~v; break;
    case 0x11c: s->irqenable = v; break;
    case 0x120: s->wakeup = v; return;
    case 0x124: s->syst = v; return;
    case 0x128:
        if (v != s->modulctrl) {
            qemu_log_mask(LOG_UNIMP, "%s: modulctrl 0x%08x\n", s->name, v);
        }
        s->modulctrl = v;
        return;
    case 0x17c: s->xferlevel = v; return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write 0x%03" HWADDR_PRIx
                      " <- 0x%08x\n", s->name, offset, v);
        return;
    }
    a8_spi_update(s);
}

/* EDMA moves 16-bit words to TX/RX, so narrow accesses must work. */
static const MemoryRegionOps a8_spi_ops = {
    .read = a8_spi_read,
    .write = a8_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void a8_spi_realize(DeviceState *dev, Error **errp)
{
    A8SpiState *s = A8_SPI(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &a8_spi_ops, s, s->name,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a8_spi_stream, s);
    a8_spi_reset(s);
    spis[s->name && !strcmp(s->name, "mcspi1")] = s;
    if (g_getenv("A8_CV_SCAN_HZ")) {
        scan_hz = MAX(g_ascii_strtod(g_getenv("A8_CV_SCAN_HZ"), NULL), 1000.0);
    }
    if (!tracef && g_getenv("A8_AUDIO_DIR")) {
        g_autofree char *p = g_strdup_printf("%s/cv.raw", g_getenv("A8_AUDIO_DIR"));
        tracef = fopen(p, "wb");
    }
}

static char *a8_spi_wave_get(Object *obj, Error **errp)
{
    return g_strdup(A8_SPI(obj)->wave_str ? A8_SPI(obj)->wave_str : "");
}

static void a8_spi_wave_set(Object *obj, const char *str, Error **errp)
{
    static const char *const names[] = { "off", "sin", "tri", "sq", "saw", "slope" };
    A8SpiState *s = A8_SPI(obj);
    char shape[8] = "";
    int ch = -1, k;
    double f = 0, a = 0, o = 2048;

    if (sscanf(str, "%d %7s %lf %lf %lf", &ch, shape, &f, &a, &o) < 2 || ch < 0 || ch > 15) {
        error_setg(errp, "wave: 'CH SHAPE FREQ AMP OFF'");
        return;
    }
    for (k = 0; k < ARRAY_SIZE(names) && strcmp(shape, names[k]); k++) {
    }
    if (k == ARRAY_SIZE(names)) {
        error_setg(errp, "wave: shape off|sin|tri|sq|saw|slope");
        return;
    }
    s->wave[ch].shape = k;
    s->wave[ch].freq = f;
    s->wave[ch].amp = a;
    s->wave[ch].off = o;
    s->wave[ch].t0 = a8_dev_s();
    g_free(s->wave_str);
    s->wave_str = g_strdup(str);
}

static char *a8_spi_fault_get(Object *obj, Error **errp)
{
    A8SpiState *s = A8_SPI(obj);

    return s->fault_pos < 0 ? g_strdup("off") : g_strdup_printf("%d 0x%04x", s->fault_pos, s->fault_word);
}

static void a8_spi_fault_set(Object *obj, const char *str, Error **errp)
{
    A8SpiState *s = A8_SPI(obj);
    int pos;
    unsigned w;

    if (!strcmp(str, "off")) {
        s->fault_pos = -1;
    } else if (sscanf(str, "%d %i", &pos, &w) == 2 && pos >= 0 && pos < 16 && w <= 0xffff) {
        s->fault_pos = pos;
        s->fault_word = w;
    } else {
        error_setg(errp, "fault: 'POS WORD' or 'off'");
    }
}

static void a8_spi_init(Object *obj)
{
    A8SpiState *s = A8_SPI(obj);

    s->fault_pos = -1;

    for (int i = 0; i < 16; i++) {
        g_autofree char *name = g_strdup_printf("ch%d", i);
        s->adc_in[i] = 2048;
        object_property_add_uint16_ptr(obj, name, &s->adc_in[i],
                                       OBJ_PROP_FLAG_READWRITE);
    }
    object_property_add_str(obj, "wave", a8_spi_wave_get, a8_spi_wave_set);
    object_property_add_str(obj, "fault", a8_spi_fault_get, a8_spi_fault_set);
}

static const Property a8_spi_props[] = {
    DEFINE_PROP_STRING("name", A8SpiState, name),
    DEFINE_PROP_INT32("tx-event", A8SpiState, tx_event, -1),
    DEFINE_PROP_INT32("rx-event", A8SpiState, rx_event, -1),
    DEFINE_PROP_LINK("edma", A8SpiState, edma, TYPE_DEVICE, DeviceState *),
};

static void a8_spi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = a8_spi_realize;
    device_class_set_props(dc, a8_spi_props);
}

static const TypeInfo a8_spi_info = {
    .name = TYPE_A8_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8SpiState),
    .instance_init = a8_spi_init,
    .class_init = a8_spi_class_init,
};

static void a8_spi_register(void)
{
    type_register_static(&a8_spi_info);
}
type_init(a8_spi_register)
