/*
 * SSD1351 128x128 RGB OLED controller on an 8080-style parallel bus, as wired
 * on the Assimil8or: GPMC CS0, address bit 18 selects data (1) or command (0).
 * Only window addressing, RAM writes, remap and display on/off/mode are
 * modelled; everything else is accepted and ignored.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "am335x.h"

#define W 128
#define H 128
#define SCALE 4
#define DC_BIT (1u << 18)

OBJECT_DECLARE_SIMPLE_TYPE(Ssd1351State, SSD1351)

struct Ssd1351State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QemuConsole *con;
    uint16_t ram[W * H];
    uint8_t cmd;
    uint8_t args[8];
    int nargs;
    uint8_t col0, col1, row0, row1, col, row;
    uint8_t remap;
    uint8_t master;
    uint8_t mode;
    bool on;
    bool have_hi;
    uint8_t hi;
    bool dirty;
    uint64_t frames;
};

static void ssd1351_pixel(Ssd1351State *s, uint16_t px)
{
    s->ram[s->row * W + s->col] = px;
    s->dirty = true;
    if (s->col < s->col1) {
        s->col++;
        return;
    }
    s->col = s->col0;
    if (s->row < s->row1) {
        s->row++;
    } else {
        s->row = s->row0;
        s->frames++;
    }
}

static void ssd1351_command(Ssd1351State *s, uint8_t c)
{
    s->cmd = c;
    s->nargs = 0;
    s->have_hi = false;
    switch (c) {
    case 0xa4: case 0xa5: case 0xa6: case 0xa7:
        s->mode = c;
        s->dirty = true;
        break;
    case 0xae: s->on = false; s->dirty = true; break;
    case 0xaf: s->on = true; s->dirty = true; break;
    }
}

static void ssd1351_data(Ssd1351State *s, uint8_t d)
{
    if (s->cmd == 0x5c) {
        if (!s->have_hi) {
            s->hi = d;
            s->have_hi = true;
        } else {
            ssd1351_pixel(s, (s->hi << 8) | d);
            s->have_hi = false;
        }
        return;
    }
    if (s->nargs < ARRAY_SIZE(s->args)) {
        s->args[s->nargs++] = d;
    }
    switch (s->cmd) {
    case 0x15:
        if (s->nargs == 2) {
            s->col0 = s->args[0] & 0x7f;
            s->col1 = s->args[1] & 0x7f;
            s->col = s->col0;
        }
        break;
    case 0x75:
        if (s->nargs == 2) {
            s->row0 = s->args[0] & 0x7f;
            s->row1 = s->args[1] & 0x7f;
            s->row = s->row0;
        }
        break;
    case 0xa0:
        s->remap = d;
        s->dirty = true;
        break;
    case 0xc7:
        s->master = d & 15;
        s->dirty = true;
        break;
    }
}

static void ssd1351_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    Ssd1351State *s = opaque;

    if (off & DC_BIT) {
        ssd1351_data(s, v);
    } else {
        ssd1351_command(s, v);
    }
}

static uint64_t ssd1351_read(void *opaque, hwaddr off, unsigned size)
{
    return 0;
}

static const MemoryRegionOps ssd1351_ops = {
    .read = ssd1351_read,
    .write = ssd1351_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/* Colour order: on the module, with A0h bit 2 (C-B-A) set as the stock app
 * sets it, the top 5 bits show as red (hardware run 4). */
static uint32_t ssd1351_rgb(Ssd1351State *s, uint16_t px)
{
    uint32_t a = (px >> 11) & 0x1f, b = (px >> 5) & 0x3f, c = px & 0x1f;
    uint32_t r = (a << 3) | (a >> 2), g = (b << 2) | (b >> 4);
    uint32_t bl = (c << 3) | (c >> 2);

    if (!(s->remap & 4)) {
        uint32_t t = r;
        r = bl;
        bl = t;
    }
    r = r * (s->master + 1) / 16;
    g = g * (s->master + 1) / 16;
    bl = bl * (s->master + 1) / 16;
    return (r << 16) | (g << 8) | bl;
}

static bool ssd1351_update(void *opaque)
{
    Ssd1351State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *dest;
    int stride;

    if (!s->dirty || surface_bits_per_pixel(surface) != 32) {
        return true;
    }
    dest = surface_data(surface);
    stride = surface_stride(surface) / 4;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int sx = x;
            uint32_t rgb;

            if (!s->on || s->mode == 0xa4) {
                rgb = 0;
            } else if (s->mode == 0xa5) {
                rgb = 0xffffff;
            } else {
                rgb = ssd1351_rgb(s, s->ram[y * W + sx]);
                if (s->mode == 0xa7) {
                    rgb ^= 0xffffff;
                }
            }
            for (int dy = 0; dy < SCALE; dy++) {
                uint32_t *row = dest + (y * SCALE + dy) * stride + x * SCALE;
                for (int dx = 0; dx < SCALE; dx++) {
                    row[dx] = rgb;
                }
            }
        }
    }
    s->dirty = false;
    qemu_console_update(s->con, 0, 0, W * SCALE, H * SCALE);
    return true;
}

static void ssd1351_invalidate(void *opaque)
{
    Ssd1351State *s = opaque;

    s->dirty = true;
}

static const GraphicHwOps ssd1351_gfx_ops = {
    .invalidate = ssd1351_invalidate,
    .gfx_update = ssd1351_update,
};

static void ssd1351_realize(DeviceState *dev, Error **errp)
{
    Ssd1351State *s = SSD1351(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &ssd1351_ops, s, "ssd1351",
                          0x1000000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->col1 = W - 1;
    s->row1 = H - 1;
    s->mode = 0xa6;
    /* State MLO leaves after drawing its logo (it then sets the flag at
     * 0x80000034 that makes the stock app skip OLED init): panel on, stock
     * remap. The logo itself is not modelled. */
    s->on = true;
    s->remap = 0x66;
    s->master = 15;
    s->con = qemu_graphic_console_create(dev, 0, &ssd1351_gfx_ops, s);
    qemu_console_resize(s->con, W * SCALE, H * SCALE);
}

static void ssd1351_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ssd1351_realize;
}

static const TypeInfo ssd1351_info = {
    .name = TYPE_SSD1351,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Ssd1351State),
    .class_init = ssd1351_class_init,
};

static void ssd1351_register(void)
{
    type_register_static(&ssd1351_info);
}
type_init(ssd1351_register)
