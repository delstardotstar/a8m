/*
 * AM335x touchscreen/ADC subsystem (TRM chapter 12), ADC use only. While the
 * module is enabled with steps enabled and FIFO0 DMA on, a timer posts one
 * result per enabled step every millisecond and raises the FIFO0 EDMA event.
 * Inputs are QOM properties ain0..ain7 (12-bit). On the Assimil8or, AIN0 is
 * the DATA2 pot.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "am335x.h"

#define NREG (0x200 / 4)
#define R(off) ((off) / 4)

#define CTRL         0x040
#define DMAENABLE_SET 0x038
#define DMAENABLE_CLR 0x03c
#define STEPENABLE   0x054
#define STEPCONFIG1  0x064
#define FIFO0COUNT   0x0e4
#define FIFO0DATA    0x100

OBJECT_DECLARE_SIMPLE_TYPE(A8TscAdcState, A8_TSCADC)

struct A8TscAdcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    DeviceState *edma;
    int32_t fifo0_event;
    uint32_t regs[NREG];
    uint32_t dmaen;
    uint16_t ain[8];
    uint32_t fifo[16];
    uint32_t last;
    int fifo_n;
    int fifo_rd;
};

static bool a8_tscadc_running(A8TscAdcState *s)
{
    return (s->regs[R(CTRL)] & 1) && (s->regs[R(STEPENABLE)] & 0x1fffe);
}

static void a8_tscadc_tick(void *opaque)
{
    A8TscAdcState *s = opaque;
    uint32_t steps = s->regs[R(STEPENABLE)] >> 1;

    if (!a8_tscadc_running(s)) {
        return;
    }
    s->fifo_n = s->fifo_rd = 0;
    for (int st = 0; st < 16 && s->fifo_n < 16; st++) {
        if (steps & (1u << st)) {
            uint32_t cfg = s->regs[R(STEPCONFIG1) + st * 2];
            int inp = (cfg >> 19) & 0xf;
            s->fifo[s->fifo_n++] = (st << 16) | (inp < 8 ? s->ain[inp] : 0);
        }
    }
    if ((s->dmaen & 1) && s->edma && s->fifo0_event >= 0) {
        a8_edma_event_now(s->edma, s->fifo0_event);
    }
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS * a8_timescale);
}

static uint64_t a8_tscadc_read(void *opaque, hwaddr off, unsigned size)
{
    A8TscAdcState *s = opaque;

    switch (off) {
    case 0x000: return 0x47300001;
    case DMAENABLE_SET:
    case DMAENABLE_CLR: return s->dmaen;
    case 0x044: return 0;           /* ADCSTAT: idle */
    case FIFO0COUNT: return s->fifo_n - s->fifo_rd;
    }
    if (off >= FIFO0DATA && off < FIFO0DATA + 0x100) {
        /* Reads pop; reading an empty FIFO repeats the last result. */
        if (s->fifo_rd < s->fifo_n) {
            s->last = s->fifo[s->fifo_rd++];
        }
        return s->last;
    }
    return s->regs[R(off)];
}

static void a8_tscadc_write(void *opaque, hwaddr off, uint64_t value,
                            unsigned size)
{
    A8TscAdcState *s = opaque;
    uint32_t v = value;

    switch (off) {
    case DMAENABLE_SET: s->dmaen |= v; break;
    case DMAENABLE_CLR: s->dmaen &= ~v; break;
    default:
        if (off < NREG * 4) {
            s->regs[R(off)] = v;
        }
    }
    if (a8_tscadc_running(s) && !timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS * a8_timescale);
    }
}

/* EDMA reads FIFO0DATA as 16-bit halves. */
static const MemoryRegionOps a8_tscadc_ops = {
    .read = a8_tscadc_read,
    .write = a8_tscadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void a8_tscadc_init(Object *obj)
{
    A8TscAdcState *s = A8_TSCADC(obj);

    for (int i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf("ain%d", i);
        s->ain[i] = 2048;
        object_property_add_uint16_ptr(obj, name, &s->ain[i],
                                       OBJ_PROP_FLAG_READWRITE);
    }
}

static void a8_tscadc_realize(DeviceState *dev, Error **errp)
{
    A8TscAdcState *s = A8_TSCADC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &a8_tscadc_ops, s,
                          "adc-tsc", 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a8_tscadc_tick, s);
}

static const Property a8_tscadc_props[] = {
    DEFINE_PROP_INT32("fifo0-event", A8TscAdcState, fifo0_event, -1),
    DEFINE_PROP_LINK("edma", A8TscAdcState, edma, TYPE_DEVICE, DeviceState *),
};

static void a8_tscadc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = a8_tscadc_realize;
    device_class_set_props(dc, a8_tscadc_props);
}

static const TypeInfo a8_tscadc_info = {
    .name = TYPE_A8_TSCADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8TscAdcState),
    .instance_init = a8_tscadc_init,
    .class_init = a8_tscadc_class_init,
};

static void a8_tscadc_register(void)
{
    type_register_static(&a8_tscadc_info);
}
type_init(a8_tscadc_register)
