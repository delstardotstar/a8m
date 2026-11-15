/*
 * AM335x MMCHS: TI wrapper registers at 0x000-0x1ff and a standard SDHCI at
 * 0x200. On this part the transfer-mode DMA bit means "raise EDMA requests",
 * not SDHCI DMA, so it is stripped before reaching the SDHCI model and each
 * full buffer raises one EDMA event instead.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/sdhci.h"
#include "exec/memattrs.h"
#include "am335x.h"

#define SDHC_PRNSTS_RBUF (1u << 11)
#define SDHC_PRNSTS_WBUF (1u << 10)

OBJECT_DECLARE_SIMPLE_TYPE(A8MmcState, A8_MMC)

struct A8MmcState {
    SysBusDevice parent_obj;
    MemoryRegion container;
    MemoryRegion ti;
    MemoryRegion proxy;
    SDHCIState sdhci;
    DeviceState *edma;
    int32_t tx_event;
    int32_t rx_event;
    uint32_t sysconfig;
    uint32_t con;
    uint32_t csre, systest, pwcnt;
    bool dma;
    bool armed;
    uint32_t consumed;
    int32_t unit;
    bool trace;
};

/* A8_MMC_TRACE=1 logs every register write (data port excepted). */
#define TRACE(s, fmt, ...) do { \
        if ((s)->trace) { \
            qemu_log("mmc%d: " fmt "\n", (s)->unit, __VA_ARGS__); \
        } \
    } while (0)

static uint32_t a8_mmc_sd_read(A8MmcState *s, hwaddr off)
{
    uint64_t v = 0;

    memory_region_dispatch_read(&s->sdhci.iomem, off, &v, MO_32,
                                MEMTXATTRS_UNSPECIFIED);
    return v;
}

static void a8_mmc_dma_check(A8MmcState *s)
{
    uint32_t st;

    if (!s->dma || !s->armed || !s->edma) {
        return;
    }
    st = a8_mmc_sd_read(s, 0x24);
    if (st & SDHC_PRNSTS_RBUF) {
        s->armed = false;
        a8_edma_event(s->edma, s->rx_event);
    } else if (st & SDHC_PRNSTS_WBUF) {
        s->armed = false;
        a8_edma_event(s->edma, s->tx_event);
    }
}

static void a8_mmc_data_access(A8MmcState *s, unsigned size)
{
    uint32_t blksize = a8_mmc_sd_read(s, 0x04) & 0xfff;

    s->consumed += size;
    if (s->consumed >= (blksize ? blksize : 512)) {
        s->consumed = 0;
        s->armed = true;
    }
}

static uint64_t a8_mmc_proxy_read(void *opaque, hwaddr off, unsigned size)
{
    A8MmcState *s = opaque;
    uint64_t v = 0;

    memory_region_dispatch_read(&s->sdhci.iomem, off, &v, size_memop(size),
                                MEMTXATTRS_UNSPECIFIED);
    if (off == 0x20) {
        a8_mmc_data_access(s, size);
        a8_mmc_dma_check(s);
    }
    return v;
}

static void a8_mmc_proxy_write(void *opaque, hwaddr off, uint64_t v,
                               unsigned size)
{
    A8MmcState *s = opaque;

    if (off != 0x20) {
        TRACE(s, "sd  0x%03x <- 0x%08x", (unsigned)(0x200 + off), (uint32_t)v);
    }
    if (off == 0x0c) {
        s->dma = v & 1;
        s->armed = true;
        s->consumed = 0;
        v &= ~1ull;
    }
    memory_region_dispatch_write(&s->sdhci.iomem, off, v, size_memop(size),
                                 MEMTXATTRS_UNSPECIFIED);
    if (off == 0x20) {
        a8_mmc_data_access(s, size);
    }
    if (off == 0x0c || off == 0x20) {
        a8_mmc_dma_check(s);
    }
}

static const MemoryRegionOps a8_mmc_proxy_ops = {
    .read = a8_mmc_proxy_read,
    .write = a8_mmc_proxy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t a8_mmc_ti_read(void *opaque, hwaddr off, unsigned size)
{
    A8MmcState *s = opaque;

    switch (off) {
    case 0x110: return s->sysconfig;
    case 0x114: return 1;
    case 0x124: return s->csre;
    case 0x128: return s->systest;
    case 0x12c: return s->con;
    case 0x130: return s->pwcnt;
    }
    qemu_log_mask(LOG_UNIMP, "mmc: read 0x%03" HWADDR_PRIx "\n", off);
    return 0;
}

static void a8_mmc_ti_write(void *opaque, hwaddr off, uint64_t v,
                            unsigned size)
{
    A8MmcState *s = opaque;

    TRACE(s, "ti  0x%03x <- 0x%08x", (unsigned)off, (uint32_t)v);
    switch (off) {
    case 0x110:
        if (v & 2) {
            device_cold_reset(DEVICE(&s->sdhci));
            s->con = 0;
            s->dma = false;
        }
        s->sysconfig = v & ~2u;
        return;
    case 0x124: s->csre = v; return;
    case 0x128: s->systest = v; return;
    case 0x12c: s->con = v; return;
    case 0x130: s->pwcnt = v; return;
    }
    qemu_log_mask(LOG_UNIMP, "mmc: write 0x%03" HWADDR_PRIx " <- 0x%08x\n",
                  off, (uint32_t)v);
}

static const MemoryRegionOps a8_mmc_ti_ops = {
    .read = a8_mmc_ti_read,
    .write = a8_mmc_ti_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void a8_mmc_init(Object *obj)
{
    A8MmcState *s = A8_MMC(obj);

    object_initialize_child(obj, "sdhci", &s->sdhci, TYPE_SYSBUS_SDHCI);
    object_property_add_alias(obj, "sd-bus", OBJECT(&s->sdhci), "sd-bus");
}

static void a8_mmc_realize(DeviceState *dev, Error **errp)
{
    A8MmcState *s = A8_MMC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->trace = getenv("A8_MMC_TRACE") != NULL;
    qdev_prop_set_uint8(DEVICE(&s->sdhci), "sd-spec-version", 2);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdhci), errp)) {
        return;
    }
    memory_region_init(&s->container, OBJECT(dev), "mmc", 0x1000);
    memory_region_init_io(&s->ti, OBJECT(dev), &a8_mmc_ti_ops, s, "mmc-ti",
                          0x200);
    memory_region_init_io(&s->proxy, OBJECT(dev), &a8_mmc_proxy_ops, s,
                          "mmc-sdhci", 0x100);
    memory_region_add_subregion(&s->container, 0, &s->ti);
    memory_region_add_subregion(&s->container, 0x200, &s->proxy);
    sysbus_init_mmio(sbd, &s->container);
    sysbus_pass_irq(sbd, SYS_BUS_DEVICE(&s->sdhci));
}

static const Property a8_mmc_props[] = {
    DEFINE_PROP_INT32("tx-event", A8MmcState, tx_event, -1),
    DEFINE_PROP_INT32("rx-event", A8MmcState, rx_event, -1),
    DEFINE_PROP_INT32("unit", A8MmcState, unit, -1),
    DEFINE_PROP_LINK("edma", A8MmcState, edma, TYPE_DEVICE, DeviceState *),
};

static void a8_mmc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = a8_mmc_realize;
    device_class_set_props(dc, a8_mmc_props);
}

static const TypeInfo a8_mmc_info = {
    .name = TYPE_A8_MMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8MmcState),
    .instance_init = a8_mmc_init,
    .class_init = a8_mmc_class_init,
};

static void a8_mmc_register(void)
{
    type_register_static(&a8_mmc_info);
}
type_init(a8_mmc_register)
