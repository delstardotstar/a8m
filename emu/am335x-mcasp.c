/*
 * AM335x McASP (TRM chapter 22), DMA-port operation only. A frame clock at
 * the "rate" property raises one TX and/or RX EDMA event per active slot
 * while the transmit/receive state machines are running. A port linked as
 * "clock-slave" of another has no clock of its own: it advances a fixed
 * number of frames per master frame (McASP1 on the Assimil8or: 4, hardware
 * run 14 counted exactly 4 McASP1 frames per McASP0 frame), spread evenly
 * through the master frame at half-step offsets, so its DMA position moves
 * by single frames as on hardware. No FIFO (AFIFO),
 * no interrupts, no bit-level timing. Receive data reads as zero unless a
 * test signal is set: "qom-set /machine/mcasp1 wave 'SLOT SHAPE FREQ AMP'"
 * (sin/tri/sq/saw/off, AMP a fraction of full scale, frame time). Signals
 * are jack voltages: as on hardware, the sample input stage and the mix
 * output stage invert (stock negates both words); channel outputs do not.
 *
 * Loopback cables: "qom-set /machine/mcasp0 loop 'MASK GAIN DCMV'" patches
 * channel output n (bit n of MASK) into CV nA (a8_mcasp_loop_cv, read by the
 * ADC model); "qom-set /machine/mcasp1 loop 'MASK GAIN DCMV'" patches mix
 * L/R (bits 0-1) into sample input L/R. Each cable scales the output by
 * GAIN and adds DCMV millivolts (word full scale = 10 V); "off" unpatches.
 *
 * Transmitted words are appended to the "out" file as raw little-endian
 * 32-bit samples, one per (serializer, slot), serializers interleaved
 * within each slot: frame = s0t0 s1t0 ... s0t1 s1t1 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "am335x.h"

#define NREG (0x2000 / 4)
#define NSER 16
#define R(off) ((off) / 4)

#define GBLCTL  0x044
#define RGBLCTL 0x060
#define XGBLCTL 0x0a0
#define RSTAT   0x080
#define XSTAT   0x0c0
#define XTDM    0x0b8
#define RTDM    0x078
#define SRCTL0  0x180

#define GBL_RFRST  (1u << 4)
#define GBL_XFRST  (1u << 12)

OBJECT_DECLARE_SIMPLE_TYPE(A8McaspState, A8_MCASP)

struct A8McaspState {
    SysBusDevice parent_obj;
    MemoryRegion cfg;
    MemoryRegion dat;
    QEMUTimer *timer;
    DeviceState *edma;
    struct A8McaspState *slave;
    int slave_frames;
    bool is_slave;
    int sub_left;
    int64_t sub_ns, sub_period;
    char *name;
    char *out;
    int32_t tx_event;
    int32_t rx_event;
    uint32_t rate;
    uint32_t regs[NREG];
    FILE *outf;
    int64_t next_ns;
    uint32_t frame[NSER * 32];
    int tx_pos;
    int rx_pos;
    uint64_t frames;
    double frame_rate;
    struct { int shape; double freq, amp; } wave[2];
    char *wave_str;
    uint32_t loop_mask;
    double loop_gain, loop_dc;
    char *loop_str;
};

/* The last transmitted frame of each port (McASP0's 8 channel words,
 * McASP1: mix L, R). */
static int32_t last_m0[8], last_m1[2];
static A8McaspState *mcasp0;

#define WORDS_PER_V (2147483648.0 / 10.0)

bool a8_mcasp_loop_cv(int n, double *volts)
{
    A8McaspState *s = mcasp0;

    if (!s || n < 0 || n > 7 || !(s->loop_mask >> n & 1)) {
        return false;
    }
    /* Words go out serializer-interleaved: ch 1,3,5,7 then 2,4,6,8. */
    *volts = s->loop_gain * last_m0[(n & 1) * 4 + n / 2] / WORDS_PER_V + s->loop_dc / 1000.0;
    return true;
}

static uint32_t a8_mcasp_mask(A8McaspState *s, int mode)
{
    uint32_t m = 0;

    for (int i = 0; i < NSER; i++) {
        if ((s->regs[R(SRCTL0) + i] & 3) == mode) {
            m |= 1u << i;
        }
    }
    return m;
}

static int a8_mcasp_slots(A8McaspState *s, hwaddr tdm)
{
    return ctpop32(s->regs[R(tdm)]);
}

/* One frame: emit the previous frame's samples, raise the next events. */
static void a8_mcasp_tick(A8McaspState *s)
{
    uint32_t gbl = s->regs[R(GBLCTL)];
    int ntx = ctpop32(a8_mcasp_mask(s, 1));
    int xslots = a8_mcasp_slots(s, XTDM);
    int rslots = a8_mcasp_slots(s, RTDM);

    if (gbl & GBL_XFRST) {
        if (s->is_slave) {
            memcpy(last_m1, s->frame, sizeof(last_m1));
        } else {
            memcpy(last_m0, s->frame, sizeof(last_m0));
        }
        if (s->outf && ntx && xslots) {
            fwrite(s->frame, 4, ntx * xslots, s->outf);
            if (!s->is_slave) {
                a8_spi_trace();
                a8_gpio_trace();
            }
        }
        memset(s->frame, 0, sizeof(s->frame));
        s->tx_pos = 0;
        s->rx_pos = 0;
        for (int i = 0; i < xslots && s->tx_event >= 0; i++) {
            a8_edma_event_now(s->edma, s->tx_event);
        }
    }
    if ((gbl & GBL_RFRST) && s->rx_event >= 0) {
        for (int i = 0; i < rslots; i++) {
            a8_edma_event_now(s->edma, s->rx_event);
        }
    }
    s->frames++;
}

static void a8_mcasp_frame(void *opaque)
{
    A8McaspState *s = opaque;
    int64_t period = NANOSECONDS_PER_SECOND * a8_timescale / s->rate;

    if (s->is_slave) {
        a8_mcasp_tick(s);
        if (--s->sub_left > 0) {
            s->sub_ns += s->sub_period;
            timer_mod(s->timer, s->sub_ns);
        }
        return;
    }
    a8_mcasp_tick(s);
    if (s->slave && s->slave_frames > 0) {
        A8McaspState *sl = s->slave;
        /* A frame still pending from the last master frame runs now. */
        for (; sl->sub_left > 0; sl->sub_left--) {
            a8_mcasp_tick(sl);
        }
        sl->sub_period = period / s->slave_frames;
        sl->sub_left = s->slave_frames;
        sl->sub_ns = s->next_ns + sl->sub_period / 2;
        timer_mod(sl->timer, sl->sub_ns);
    }
    if (s->regs[R(GBLCTL)] & (GBL_XFRST | GBL_RFRST)) {
        s->next_ns += period;
        timer_mod(s->timer, s->next_ns);
    }
}

static void a8_mcasp_gbl(A8McaspState *s, uint32_t v)
{
    uint32_t was = s->regs[R(GBLCTL)];

    if (v != was) {
        qemu_log_mask(LOG_UNIMP, "%s: gblctl 0x%04x\n", s->name, v);
    }
    s->regs[R(GBLCTL)] = v;
    if (!(was & (GBL_XFRST | GBL_RFRST)) && (v & (GBL_XFRST | GBL_RFRST))) {
        qemu_log_mask(LOG_UNIMP, "%s: running (gblctl 0x%04x, %d tx ser, "
                      "%d/%d slots)\n", s->name, v,
                      ctpop32(a8_mcasp_mask(s, 1)),
                      a8_mcasp_slots(s, XTDM), a8_mcasp_slots(s, RTDM));
        if (!s->is_slave) {
            s->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            /* From the timer, not here: DMA into our own data port from
             * inside our MMIO handler is blocked as re-entrant. */
            timer_mod(s->timer, s->next_ns);
        }
    }
}

static uint64_t a8_mcasp_cfg_read(void *opaque, hwaddr off, unsigned size)
{
    A8McaspState *s = opaque;

    switch (off) {
    case RGBLCTL:
    case XGBLCTL:
        return s->regs[R(GBLCTL)];
    case RSTAT:
    case XSTAT:
        return 0;
    }
    if (off >= SRCTL0 && off < SRCTL0 + NSER * 4) {
        uint32_t v = s->regs[R(off)];
        return (v & 3) == 1 ? v | 0x10 : v;
    }
    return s->regs[R(off)];
}

static void a8_mcasp_cfg_write(void *opaque, hwaddr off, uint64_t value,
                               unsigned size)
{
    A8McaspState *s = opaque;
    uint32_t v = value;
    uint32_t g = s->regs[R(GBLCTL)];

    switch (off) {
    case GBLCTL: a8_mcasp_gbl(s, v); return;
    case RGBLCTL: a8_mcasp_gbl(s, (g & ~0xffu) | (v & 0xff)); return;
    case XGBLCTL: a8_mcasp_gbl(s, (g & ~0xff00u) | (v & 0xff00)); return;
    case RSTAT:
    case XSTAT:
        return;
    }
    s->regs[R(off)] = v;
}

static const MemoryRegionOps a8_mcasp_cfg_ops = {
    .read = a8_mcasp_cfg_read,
    .write = a8_mcasp_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t a8_mcasp_dat_read(void *opaque, hwaddr off, unsigned size)
{
    A8McaspState *s = opaque;
    int slot = s->rx_pos++ & 1;
    double ph, x, y = 0;

    if (s->loop_mask >> slot & 1) {
        y = -s->loop_gain * last_m1[slot] + s->loop_dc / 1000.0 * WORDS_PER_V;
    }
    if (!s->wave[slot].shape) {
        return (uint32_t)(int32_t)MIN(MAX(llround(-y), -2147483647LL), 2147483647LL);
    }
    ph = (double)s->frames / s->frame_rate * s->wave[slot].freq;
    ph -= floor(ph);
    switch (s->wave[slot].shape) {
    case 1: x = sin(2 * M_PI * ph); break;
    case 2: x = ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph; break;
    case 3: x = ph < 0.5 ? 1 : -1; break;
    default: x = 2 * ph - 1; break;
    }
    x = -(x * s->wave[slot].amp * 2147483647.0 + y);
    return (uint32_t)(int32_t)MIN(MAX(lround(x), -2147483647L), 2147483647L);
}

static char *a8_mcasp_wave_get(Object *obj, Error **errp)
{
    return g_strdup(A8_MCASP(obj)->wave_str ? A8_MCASP(obj)->wave_str : "");
}

static void a8_mcasp_wave_set(Object *obj, const char *str, Error **errp)
{
    static const char *const names[] = { "off", "sin", "tri", "sq", "saw" };
    A8McaspState *s = A8_MCASP(obj);
    char shape[8] = "";
    int slot = -1, k;
    double f = 0, a = 0;

    if (sscanf(str, "%d %7s %lf %lf", &slot, shape, &f, &a) < 2 || slot < 0 || slot > 1) {
        error_setg(errp, "wave: 'SLOT SHAPE FREQ AMP'");
        return;
    }
    for (k = 0; k < ARRAY_SIZE(names) && strcmp(shape, names[k]); k++) {
    }
    if (k == ARRAY_SIZE(names)) {
        error_setg(errp, "wave: shape off|sin|tri|sq|saw");
        return;
    }
    s->wave[slot].shape = k;
    s->wave[slot].freq = f;
    s->wave[slot].amp = a;
    g_free(s->wave_str);
    s->wave_str = g_strdup(str);
}

static char *a8_mcasp_loop_get(Object *obj, Error **errp)
{
    return g_strdup(A8_MCASP(obj)->loop_str ? A8_MCASP(obj)->loop_str : "off");
}

static void a8_mcasp_loop_set(Object *obj, const char *str, Error **errp)
{
    A8McaspState *s = A8_MCASP(obj);
    unsigned m = 0;
    double g = 1, dc = 0;

    if (strcmp(str, "off") && sscanf(str, "%i %lf %lf", &m, &g, &dc) < 1) {
        error_setg(errp, "loop: 'MASK GAIN DCMV' or 'off'");
        return;
    }
    s->loop_mask = m;
    s->loop_gain = g;
    s->loop_dc = dc;
    g_free(s->loop_str);
    s->loop_str = g_strdup(str);
}

static void a8_mcasp_dat_write(void *opaque, hwaddr off, uint64_t value,
                               unsigned size)
{
    A8McaspState *s = opaque;

    if (s->tx_pos < ARRAY_SIZE(s->frame)) {
        s->frame[s->tx_pos++] = value;
    }
}

static const MemoryRegionOps a8_mcasp_dat_ops = {
    .read = a8_mcasp_dat_read,
    .write = a8_mcasp_dat_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void a8_mcasp_realize(DeviceState *dev, Error **errp)
{
    A8McaspState *s = A8_MCASP(dev);

    memory_region_init_io(&s->cfg, OBJECT(dev), &a8_mcasp_cfg_ops, s,
                          s->name, 0x2000);
    memory_region_init_io(&s->dat, OBJECT(dev), &a8_mcasp_dat_ops, s,
                          "mcasp-dat", 0x400000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->cfg);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->dat);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a8_mcasp_frame, s);
    s->frame_rate = s->rate;
    object_property_add_str(OBJECT(dev), "wave", a8_mcasp_wave_get, a8_mcasp_wave_set);
    object_property_add_str(OBJECT(dev), "loop", a8_mcasp_loop_get, a8_mcasp_loop_set);
    if (s->name && !strcmp(s->name, "mcasp0")) {
        mcasp0 = s;
    }
    if (s->out && *s->out) {
        s->outf = fopen(s->out, "wb");
        if (!s->outf) {
            error_setg_errno(errp, errno, "%s: cannot open %s", s->name,
                             s->out);
        }
    }
}

/* Links a port that runs frames_per_master frames per master frame. */
void a8_mcasp_set_clock_slave(DeviceState *master, DeviceState *slave,
                              int frames_per_master)
{
    A8_MCASP(master)->slave = A8_MCASP(slave);
    A8_MCASP(master)->slave_frames = frames_per_master;
    A8_MCASP(slave)->is_slave = true;
    A8_MCASP(slave)->frame_rate = (double)A8_MCASP(master)->rate * frames_per_master;
}

static const Property a8_mcasp_props[] = {
    DEFINE_PROP_STRING("name", A8McaspState, name),
    DEFINE_PROP_STRING("out", A8McaspState, out),
    DEFINE_PROP_INT32("tx-event", A8McaspState, tx_event, -1),
    DEFINE_PROP_INT32("rx-event", A8McaspState, rx_event, -1),
    DEFINE_PROP_UINT32("rate", A8McaspState, rate, 48000),
    DEFINE_PROP_LINK("edma", A8McaspState, edma, TYPE_DEVICE, DeviceState *),
};

static void a8_mcasp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = a8_mcasp_realize;
    device_class_set_props(dc, a8_mcasp_props);
}

static const TypeInfo a8_mcasp_info = {
    .name = TYPE_A8_MCASP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8McaspState),
    .class_init = a8_mcasp_class_init,
};

static void a8_mcasp_register(void)
{
    type_register_static(&a8_mcasp_info);
}
type_init(a8_mcasp_register)
