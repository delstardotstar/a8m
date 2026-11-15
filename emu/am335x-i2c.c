/*
 * AM335x I2C master (TRM chapter 21), polled/IRQ, no FIFO thresholds or DMA.
 * Every address ACKs; read bytes come from a8_i2c_respond(). Each transfer
 * is logged as one line when it completes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "am335x.h"

#define IRQ_ARDY (1u << 2)
#define IRQ_RRDY (1u << 3)
#define IRQ_XRDY (1u << 4)
#define IRQ_BF   (1u << 8)
#define IRQ_BB   (1u << 12)

#define CON_STT 1u
#define CON_STP 2u
#define CON_TRX (1u << 9)

OBJECT_DECLARE_SIMPLE_TYPE(A8I2cState, A8_I2C)

struct A8I2cState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    char *name;
    uint32_t raw;
    uint32_t enable;
    uint32_t con, cnt, sa, oa, psc, scll, sclh, buf, sysc, we;
    uint32_t left;
    GString *line;
};

static uint8_t a8_i2c_respond(A8I2cState *s, uint8_t addr)
{
    return 0;
}

static void a8_i2c_update(A8I2cState *s)
{
    qemu_set_irq(s->irq, !!(s->raw & s->enable & ~IRQ_BB));
}

static void a8_i2c_reset(A8I2cState *s)
{
    s->raw = IRQ_BF;
    s->enable = 0;
    s->con = s->cnt = s->sa = s->oa = s->psc = 0;
    s->scll = s->sclh = s->buf = s->sysc = s->we = 0;
    s->left = 0;
    g_string_truncate(s->line, 0);
}

static void a8_i2c_byte_done(A8I2cState *s)
{
    if (--s->left) {
        s->raw |= (s->con & CON_TRX) ? IRQ_XRDY : IRQ_RRDY;
        return;
    }
    s->raw = (s->raw & ~(IRQ_XRDY | IRQ_RRDY)) | IRQ_ARDY;
    qemu_log_mask(LOG_UNIMP, "%s: %s 0x%02x:%s%s\n", s->name,
                  (s->con & CON_TRX) ? "W" : "R", s->sa, s->line->str,
                  (s->con & CON_STP) ? "" : " (no stop)");
    g_string_truncate(s->line, 0);
    if (s->con & CON_STP) {
        s->raw = (s->raw & ~IRQ_BB) | IRQ_BF;
    }
    s->con &= ~(CON_STT | CON_STP);
}

static void a8_i2c_start(A8I2cState *s)
{
    s->left = s->cnt ? s->cnt : 65536;
    s->raw = (s->raw & ~IRQ_BF) | IRQ_BB;
    s->raw |= (s->con & CON_TRX) ? IRQ_XRDY : IRQ_RRDY;
}

static uint64_t a8_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    A8I2cState *s = opaque;

    switch (offset) {
    case 0x00: return 0x50400000;
    case 0x10: return s->sysc;
    case 0x24: return s->raw;
    case 0x28: return s->raw & s->enable;
    case 0x2c:
    case 0x30: return s->enable;
    case 0x34: return s->we;
    case 0x90: return 1;
    case 0x94: return s->buf;
    case 0x98: return s->cnt;
    case 0x9c: {
        uint8_t b;
        if (!s->left || (s->con & CON_TRX)) {
            return 0;
        }
        b = a8_i2c_respond(s, s->sa);
        g_string_append_printf(s->line, " %02x", b);
        s->raw &= ~IRQ_RRDY;
        a8_i2c_byte_done(s);
        a8_i2c_update(s);
        return b;
    }
    case 0xa4: return s->con;
    case 0xa8: return s->oa;
    case 0xac: return s->sa;
    case 0xb0: return s->psc;
    case 0xb4: return s->scll;
    case 0xb8: return s->sclh;
    case 0xc0: return 0;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read 0x%03" HWADDR_PRIx "\n",
                  s->name, offset);
    return 0;
}

static void a8_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    A8I2cState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case 0x10:
        if (v & 2) {
            a8_i2c_reset(s);
        } else {
            s->sysc = v;
        }
        return;
    case 0x24: s->raw |= v & ~IRQ_BB; break;
    case 0x28:
        s->raw &= ~v | IRQ_BB;
        if (s->left) {
            s->raw |= (s->con & CON_TRX) ? IRQ_XRDY : IRQ_RRDY;
        }
        break;
    case 0x2c: s->enable |= v; break;
    case 0x30: s->enable &= ~v; break;
    case 0x34: s->we = v; return;
    case 0x94: s->buf = v; return;
    case 0x98: s->cnt = v & 0xffff; return;
    case 0x9c:
        if (s->left && (s->con & CON_TRX)) {
            g_string_append_printf(s->line, " %02x", v & 0xff);
            s->raw &= ~IRQ_XRDY;
            a8_i2c_byte_done(s);
        }
        break;
    case 0xa4:
        s->con = v;
        if (!(v & (1u << 15))) {
            s->raw = IRQ_BF;
            s->left = 0;
        } else if (v & CON_STT) {
            a8_i2c_start(s);
        } else if ((v & CON_STP) && !s->left) {
            s->raw = (s->raw & ~IRQ_BB) | IRQ_BF;
            s->con &= ~CON_STP;
        }
        break;
    case 0xa8: s->oa = v; return;
    case 0xac: s->sa = v & 0x3ff; return;
    case 0xb0: s->psc = v; return;
    case 0xb4: s->scll = v; return;
    case 0xb8: s->sclh = v; return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write 0x%03" HWADDR_PRIx
                      " <- 0x%08x\n", s->name, offset, v);
        return;
    }
    a8_i2c_update(s);
}

static const MemoryRegionOps a8_i2c_ops = {
    .read = a8_i2c_read,
    .write = a8_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void a8_i2c_realize(DeviceState *dev, Error **errp)
{
    A8I2cState *s = A8_I2C(dev);

    s->line = g_string_new(NULL);
    memory_region_init_io(&s->iomem, OBJECT(dev), &a8_i2c_ops, s, s->name,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    a8_i2c_reset(s);
}

static const Property a8_i2c_props[] = {
    DEFINE_PROP_STRING("name", A8I2cState, name),
};

static void a8_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = a8_i2c_realize;
    device_class_set_props(dc, a8_i2c_props);
}

static const TypeInfo a8_i2c_info = {
    .name = TYPE_A8_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8I2cState),
    .class_init = a8_i2c_class_init,
};

static void a8_i2c_register(void)
{
    type_register_static(&a8_i2c_info);
}
type_init(a8_i2c_register)
