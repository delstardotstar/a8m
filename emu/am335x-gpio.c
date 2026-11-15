/*
 * AM335x GPIO bank (TRM chapter 25): 32 pins, direction, set/clear,
 * level/edge interrupt detection on two lines. Input levels start from the
 * "inputs" property and can be changed at runtime through the "in" QOM
 * property (e.g. "qom-set /machine/gpio1 in 0xffffffff") or qdev GPIO
 * inputs. Output changes are logged. With $A8_AUDIO_DIR set, gpio0.raw gets
 * gpio0's input levels (u32) per McASP0 frame, in step with mcasp0.raw.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qapi/visitor.h"
#include "am335x.h"

OBJECT_DECLARE_SIMPLE_TYPE(A8GpioState, A8_GPIO)

struct A8GpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[2];
    char *name;
    uint32_t inputs;
    uint32_t in;
    uint32_t sysconfig, eoi, ctrl;
    uint32_t raw, enable[2], wake[2];
    uint32_t oe, out;
    uint32_t level[2], rising, falling, debenable, debtime;
};

static A8GpioState *gpio0;
static FILE *tracef;

void a8_gpio_trace(void)
{
    if (tracef && gpio0) {
        fwrite(&gpio0->in, 4, 1, tracef);
    }
}

static uint32_t a8_gpio_datain(A8GpioState *s)
{
    return (s->out & ~s->oe) | (s->in & s->oe);
}

static void a8_gpio_update(A8GpioState *s)
{
    uint32_t din = a8_gpio_datain(s);

    s->raw |= (s->level[0] & ~din) | (s->level[1] & din);
    for (int i = 0; i < 2; i++) {
        qemu_set_irq(s->irq[i], !!(s->raw & s->enable[i]));
    }
}

static void a8_gpio_set_input(A8GpioState *s, uint32_t in)
{
    uint32_t old = a8_gpio_datain(s);
    uint32_t now;

    s->in = in;
    now = a8_gpio_datain(s);
    s->raw |= (s->rising & now & ~old) | (s->falling & ~now & old);
    a8_gpio_update(s);
}

static void a8_gpio_pin(void *opaque, int n, int level)
{
    A8GpioState *s = opaque;

    a8_gpio_set_input(s, level ? s->in | (1u << n) : s->in & ~(1u << n));
}

static void a8_gpio_set_out(A8GpioState *s, uint32_t out)
{
    uint32_t changed = (out ^ s->out) & ~s->oe;

    for (uint32_t m = changed; m; m &= m - 1) {
        int pin = ctz32(m);
        qemu_log_mask(LOG_UNIMP, "%s.%d = %d\n", s->name, pin,
                      !!(out & (1u << pin)));
    }
    s->out = out;
}

static uint64_t a8_gpio_read(void *opaque, hwaddr off, unsigned size)
{
    A8GpioState *s = opaque;

    switch (off) {
    case 0x000: return 0x50600801;
    case 0x010: return s->sysconfig;
    case 0x020: return s->eoi;
    case 0x024: return s->raw;
    case 0x028: return s->raw;
    case 0x02c: return s->raw & s->enable[0];
    case 0x030: return s->raw & s->enable[1];
    case 0x034: case 0x038: return s->enable[(off - 0x034) / 4];
    case 0x03c: case 0x040: return s->enable[(off - 0x03c) / 4];
    case 0x044: case 0x048: return s->wake[(off - 0x044) / 4];
    case 0x114: return 1;
    case 0x130: return s->ctrl;
    case 0x134: return s->oe;
    case 0x138: return a8_gpio_datain(s);
    case 0x13c: return s->out;
    case 0x140: case 0x144: return s->level[(off - 0x140) / 4];
    case 0x148: return s->rising;
    case 0x14c: return s->falling;
    case 0x150: return s->debenable;
    case 0x154: return s->debtime;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read 0x%03" HWADDR_PRIx "\n", s->name, off);
    return 0;
}

static void a8_gpio_write(void *opaque, hwaddr off, uint64_t value,
                          unsigned size)
{
    A8GpioState *s = opaque;
    uint32_t v = value;

    switch (off) {
    case 0x010:
        if (v & 2) {
            s->raw = s->enable[0] = s->enable[1] = 0;
            s->level[0] = s->level[1] = s->rising = s->falling = 0;
            s->oe = 0xffffffff;
            s->out = 0;
        }
        s->sysconfig = v & ~2u;
        break;
    case 0x020: s->eoi = v; break;
    case 0x024: s->raw |= v; break;
    case 0x02c: case 0x030: s->raw &= ~v; break;
    case 0x034: case 0x038: s->enable[(off - 0x034) / 4] |= v; break;
    case 0x03c: case 0x040: s->enable[(off - 0x03c) / 4] &= ~v; break;
    case 0x044: case 0x048: s->wake[(off - 0x044) / 4] = v; return;
    case 0x130: s->ctrl = v; return;
    case 0x134: s->oe = v; break;
    case 0x13c: a8_gpio_set_out(s, v); break;
    case 0x140: case 0x144: s->level[(off - 0x140) / 4] = v; break;
    case 0x148: s->rising = v; break;
    case 0x14c: s->falling = v; break;
    case 0x150: s->debenable = v; return;
    case 0x154: s->debtime = v; return;
    case 0x190: a8_gpio_set_out(s, s->out & ~v); break;
    case 0x194: a8_gpio_set_out(s, s->out | v); break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write 0x%03" HWADDR_PRIx " <- 0x%08x\n",
                      s->name, off, v);
        return;
    }
    a8_gpio_update(s);
}

static const MemoryRegionOps a8_gpio_ops = {
    .read = a8_gpio_read,
    .write = a8_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void a8_gpio_realize(DeviceState *dev, Error **errp)
{
    A8GpioState *s = A8_GPIO(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &a8_gpio_ops, s, s->name,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[0]);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[1]);
    qdev_init_gpio_in(dev, a8_gpio_pin, 32);
    s->oe = 0xffffffff;
    s->in = s->inputs;
    if (s->name && !strcmp(s->name, "gpio0")) {
        gpio0 = s;
        if (!tracef && g_getenv("A8_AUDIO_DIR")) {
            g_autofree char *p = g_strdup_printf("%s/gpio0.raw", g_getenv("A8_AUDIO_DIR"));
            tracef = fopen(p, "wb");
        }
    }
}

static void a8_gpio_get_in(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    A8GpioState *s = A8_GPIO(obj);

    visit_type_uint32(v, name, &s->in, errp);
}

static void a8_gpio_set_in(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    A8GpioState *s = A8_GPIO(obj);
    uint32_t val;

    if (visit_type_uint32(v, name, &val, errp)) {
        a8_gpio_set_input(s, val);
    }
}

static void a8_gpio_init(Object *obj)
{
    object_property_add(obj, "in", "uint32", a8_gpio_get_in, a8_gpio_set_in,
                        NULL, NULL);
}

static const Property a8_gpio_props[] = {
    DEFINE_PROP_STRING("name", A8GpioState, name),
    DEFINE_PROP_UINT32("inputs", A8GpioState, inputs, 0),
};

static void a8_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = a8_gpio_realize;
    device_class_set_props(dc, a8_gpio_props);
}

static const TypeInfo a8_gpio_info = {
    .name = TYPE_A8_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8GpioState),
    .instance_init = a8_gpio_init,
    .class_init = a8_gpio_class_init,
};

static void a8_gpio_register(void)
{
    type_register_static(&a8_gpio_info);
}
type_init(a8_gpio_register)
