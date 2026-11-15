/*
 * Assimil8or (Olimex AM3352-SOM) machine: Cortex-A8, 512 MB DDR, and every
 * AM335x peripheral as a logging stub until it gets a real model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/machines-qom.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "target/arm/cpu.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "hw/core/irq.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "hw/char/serial-mm.h"
#include "system/system.h"
#include "system/runstate.h"
#include "am335x.h"

#define A8_DDR_BASE 0x80000000ULL

double a8_timescale = 1;

/*
 * Register file: reads return the last written value with `set` bits forced
 * on and `clr` bits forced off. Repeated identical reads are collapsed in the
 * log so polling loops stay readable.
 */
typedef struct {
    hwaddr offset;
    uint32_t set;
    uint32_t clr;
} A8Rule;

typedef struct {
    const char *name;
    const A8Rule *rules;
    void (*init)(uint32_t *regs);
    uint32_t (*fixup)(const uint32_t *regs, hwaddr offset, uint32_t value);
    void (*write)(hwaddr offset, uint32_t value);
} A8RegsSpec;

#define TYPE_A8_REGS "a8-regs"
OBJECT_DECLARE_SIMPLE_TYPE(A8RegsState, A8_REGS)

struct A8RegsState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    const A8RegsSpec *spec;
    uint32_t *regs;
    hwaddr last_offset;
    uint64_t repeats;
};

static void a8_regs_flush(A8RegsState *s)
{
    if (s->repeats) {
        qemu_log_mask(LOG_UNIMP, "%s: read 0x%03" HWADDR_PRIx
                      " repeated %" PRIu64 " times\n",
                      s->spec->name, s->last_offset, s->repeats);
        s->repeats = 0;
    }
}

static uint64_t a8_regs_read(void *opaque, hwaddr offset, unsigned size)
{
    A8RegsState *s = opaque;
    uint32_t v = s->regs[offset / 4];

    for (const A8Rule *r = s->spec->rules; r && (r->set || r->clr); r++) {
        if (r->offset == offset) {
            v = (v | r->set) & ~r->clr;
        }
    }
    if (s->spec->fixup) {
        v = s->spec->fixup(s->regs, offset, v);
    }
    if (offset == s->last_offset && s->repeats != UINT64_MAX) {
        s->repeats++;
        return v;
    }
    a8_regs_flush(s);
    s->last_offset = offset;
    qemu_log_mask(LOG_UNIMP, "%s: read  0x%03" HWADDR_PRIx " -> 0x%08x\n",
                  s->spec->name, offset, v);
    return v;
}

static void a8_regs_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    A8RegsState *s = opaque;

    a8_regs_flush(s);
    s->last_offset = (hwaddr)-1;
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%03" HWADDR_PRIx " <- 0x%08x\n",
                  s->spec->name, offset, (uint32_t)value);
    s->regs[offset / 4] = value;
    if (s->spec->write) {
        s->spec->write(offset, value);
    }
}

static const MemoryRegionOps a8_regs_ops = {
    .read = a8_regs_read,
    .write = a8_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const TypeInfo a8_regs_info = {
    .name = TYPE_A8_REGS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8RegsState),
};

static void a8_regs_register(void)
{
    type_register_static(&a8_regs_info);
}
type_init(a8_regs_register)

static void a8_map_regs(const A8RegsSpec *spec, hwaddr base, hwaddr size)
{
    DeviceState *dev = qdev_new(TYPE_A8_REGS);
    A8RegsState *s = A8_REGS(dev);

    s->spec = spec;
    s->regs = g_new0(uint32_t, size / 4);
    if (spec->init) {
        spec->init(s->regs);
    }
    s->last_offset = (hwaddr)-1;
    memory_region_init_io(&s->iomem, OBJECT(dev), &a8_regs_ops, s,
                          spec->name, size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, base, -500);
}

/*
 * PRCM clocks. A domain's activity bits read active only while it is awake
 * (CLKTRCTRL = SW_WKUP), and a module reads functional (IDLEST 0) only while
 * its MODULEMODE is enabled and its domain is awake; otherwise IDLEST reads
 * disabled, so a wait for it never ends, as on hardware.
 *
 * Initial state is what MLO leaves for app: every domain awake except L3S.
 * Observed on the module: L4LS awake (app 8bf09bcf enabled Timer2 and
 * GPIO2), L3S asleep (enabling GPMC hung until app woke L3S). Stock app
 * only polls the always-on domains (e.g. L3_AON) and never wakes them.
 */
#define CLKTRCTRL_SW_WKUP 2u
#define IDLEST_DISABLED   0x00030000u

enum {
    DOM_L4LS = 0x000, DOM_L3S = 0x004, DOM_L3 = 0x00c, DOM_OCPWP = 0x12c,
    DOM_WKUP = 0x400,
};

static const struct { uint16_t clkctrl, domain; } a8_modules[] = {
    /* L4LS */
    { 0x0ac, DOM_L4LS }, { 0x0b0, DOM_L4LS }, { 0x0b4, DOM_L4LS }, /* GPIO1-3 */
    { 0x080, DOM_L4LS }, { 0x084, DOM_L4LS }, { 0x088, DOM_L4LS }, /* TIMER2-4 */
    { 0x0ec, DOM_L4LS }, { 0x0f0, DOM_L4LS }, { 0x07c, DOM_L4LS }, /* TIMER5-7 */
    { 0x04c, DOM_L4LS }, { 0x050, DOM_L4LS },                      /* McSPI0/1 */
    { 0x03c, DOM_L4LS }, { 0x0f4, DOM_L4LS },                      /* MMC0/1 */
    { 0x048, DOM_L4LS }, { 0x044, DOM_L4LS },                      /* I2C1/2 */
    { 0x060, DOM_L4LS },                                           /* L4LS */
    /* L3S */
    { 0x030, DOM_L3S }, { 0x034, DOM_L3S }, { 0x068, DOM_L3S },    /* GPMC, McASP0/1 */
    { 0x0f8, DOM_L3S },                                            /* MMC2 */
    /* L3 */
    { 0x0bc, DOM_L3 }, { 0x024, DOM_L3 }, { 0x0fc, DOM_L3 }, { 0x100, DOM_L3 }, /* EDMA */
    { 0x0dc, DOM_L3 }, { 0x0e0, DOM_L3 },                          /* L3_INSTR, L3 */
    /* OCPWP */
    { 0x130, DOM_OCPWP },
    /* WKUP */
    { 0x408, DOM_WKUP }, { 0x4bc, DOM_WKUP }, { 0x4b8, DOM_WKUP }, /* GPIO0, ADC, I2C0 */
    { 0x4b4, DOM_WKUP }, { 0x410, DOM_WKUP }, { 0x4c4, DOM_WKUP }, /* UART0, TIMER0/1 */
};

static bool a8_domain_awake(const uint32_t *regs, hwaddr dom)
{
    return (regs[dom / 4] & 3) == CLKTRCTRL_SW_WKUP;
}

static const uint16_t a8_domains[] = {
    DOM_L4LS, DOM_L3S, DOM_L3, DOM_OCPWP, DOM_WKUP,
    0x11c, 0x140, 0x144, 0x148, 0x14c, 0x418, 0x4cc, 0x600, 0x804, 0x900,
    0x90c, 0xa00,
};

/*
 * PRM_RSTST (PRM_DEVICE + 0x08), write 1 to clear: the last reset's cause,
 * from $A8_RSTST (emu/run.sh: 0x10 WDT1_RST after a watchdog reset, else
 * 1 GLOBAL_COLD_RST).
 */
#define PRM_RSTST 0xf08
static uint32_t a8_rstst = 1;

static void a8_prcm_init(uint32_t *regs)
{
    if (g_getenv("A8_RSTST")) {
        a8_rstst = (uint32_t)g_ascii_strtoull(g_getenv("A8_RSTST"), NULL, 0);
    }
    for (size_t i = 0; i < ARRAY_SIZE(a8_domains); i++) {
        if (a8_domains[i] != DOM_L3S) {
            regs[a8_domains[i] / 4] = CLKTRCTRL_SW_WKUP;
        }
    }
}

static uint32_t a8_prcm_fixup(const uint32_t *regs, hwaddr off, uint32_t v)
{
    if (off == PRM_RSTST) {
        return a8_rstst;
    }
    for (size_t i = 0; i < ARRAY_SIZE(a8_domains); i++) {
        if (off == a8_domains[i]) {
            return a8_domain_awake(regs, off) ? v | 0xfffffffc : v & 3;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(a8_modules); i++) {
        if (off == a8_modules[i].clkctrl) {
            bool on = (v & 3) == 2 && a8_domain_awake(regs, a8_modules[i].domain);
            return (v & ~IDLEST_DISABLED) | (on ? 0 : IDLEST_DISABLED);
        }
    }
    return v;
}

/*
 * PRM_RSTCTRL (PRM_DEVICE + 0x00): a global warm or cold software reset
 * re-runs the boot from power-on. QEMU exits with status 4 and emu/run.sh
 * boots again the way MLO does (emu/mlo.py reads app from the SOM card).
 * A8_RESET=0 ignores the write (a reset that never comes).
 */
#define PRM_RSTCTRL 0xf00
#define A8_EXIT_RESET 4

static void a8_prcm_write(hwaddr off, uint32_t v)
{
    if (off == PRM_RSTST) {
        a8_rstst &= ~v;
    }
    if (off == PRM_RSTCTRL && (v & 3) && g_strcmp0(g_getenv("A8_RESET"), "0")) {
        fprintf(stderr, "a8: %s software reset (PRM_RSTCTRL)\n",
                v & 1 ? "warm" : "cold");
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_RESET,
                                               A8_EXIT_RESET);
    }
}

/* DPLL IDLEST reads as locked. */
static const A8Rule cm_per_rules[] = {
    { 0x420, 1, 0 },        /* CM_IDLEST_DPLL_MPU */
    { 0x434, 1, 0 },        /* CM_IDLEST_DPLL_DDR */
    { 0x448, 1, 0 },        /* CM_IDLEST_DPLL_DISP */
    { 0x45c, 1, 0 },        /* CM_IDLEST_DPLL_CORE */
    { 0x470, 1, 0 },        /* CM_IDLEST_DPLL_PER */
    { 0 }
};

static const A8Rule uart_rules[] = {
    { 0x014, 0x60, 0 },     /* LSR: THR and transmitter empty */
    { 0x058, 1, 0 },        /* SYSS: reset done */
    { 0 }
};
static const A8Rule gpmc_rules[] = { { 0x014, 1, 0 }, { 0 } };
static const A8Rule emif_rules[] = { { 0x004, 4, 0 }, { 0 } };

typedef struct {
    A8RegsSpec spec;
    hwaddr base;
    hwaddr size;
} A8Region;

/* AM335x TRM (SPRUH73) chapter 2 memory map. */
static const A8Region a8_regions[] = {
    { { "boot-rom" },           0x40000000, 0x30000 },
    { { "l3f-cfg" },            0x44000000, 0x400000 },
    { { "prcm", cm_per_rules, a8_prcm_init, a8_prcm_fixup, a8_prcm_write }, 0x44e00000, 0x2000 },
    { { "uart0", uart_rules },  0x44e09000, 0x1000 },
    { { "control" },            0x44e10000, 0x2000 },
    { { "ddr-phy" },            0x44e12000, 0x1000 },
    { { "rtc" },                0x44e3e000, 0x1000 },
    { { "usbss" },              0x47400000, 0x8000 },
    { { "uart1", uart_rules },  0x48022000, 0x1000 },
    { { "uart2", uart_rules },  0x48024000, 0x1000 },
    { { "elm" },                0x48080000, 0x2000 },
    { { "mailbox" },            0x480c8000, 0x1000 },
    { { "spinlock" },           0x480ca000, 0x1000 },
    { { "uart3", uart_rules },  0x481a6000, 0x1000 },
    { { "uart4", uart_rules },  0x481a8000, 0x1000 },
    { { "uart5", uart_rules },  0x481aa000, 0x1000 },
    { { "dcan0" },              0x481cc000, 0x2000 },
    { { "dcan1" },              0x481d0000, 0x2000 },
    { { "pwmss0" },             0x48300000, 0x1000 },
    { { "pwmss1" },             0x48302000, 0x1000 },
    { { "pwmss2" },             0x48304000, 0x1000 },
    { { "lcdc" },               0x4830e000, 0x1000 },
    { { "edma-tptc0" },         0x49800000, 0x100000 },
    { { "edma-tptc1" },         0x49900000, 0x100000 },
    { { "edma-tptc2" },         0x49a00000, 0x100000 },
    { { "l4-fast" },            0x4a000000, 0x100000 },
    { { "cpsw" },               0x4a100000, 0x8000 },
    { { "pru-icss" },           0x4a300000, 0x80000 },
    { { "debugss" },            0x4b000000, 0x1000000 },
    { { "emif0", emif_rules },  0x4c000000, 0x1000000 },
    { { "gpmc", gpmc_rules },               0x50000000, 0x1000000 },
    { { "sgx" },                0x56000000, 0x1000000 },
};

static void a8_map_ram(const char *name, hwaddr base, hwaddr size)
{
    MemoryRegion *mr = g_new(MemoryRegion, 1);

    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

static void a8_map_dev(const char *type, const char *name, hwaddr base,
                       qemu_irq irq)
{
    DeviceState *dev = qdev_new(type);

    qdev_prop_set_string(dev, "name", name);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
}

/* Reachable in the monitor as /machine/<name> (MAX11128 inputs ch0..ch15). */
static void a8_map_spi(MachineState *machine, const char *name, hwaddr base,
                       qemu_irq irq, DeviceState *edma, int tx_event)
{
    DeviceState *dev = qdev_new(TYPE_A8_SPI);

    object_property_add_child(OBJECT(machine), name, OBJECT(dev));
    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_int32(dev, "tx-event", tx_event);
    qdev_prop_set_int32(dev, "rx-event", tx_event + 1);
    object_property_set_link(OBJECT(dev), "edma", OBJECT(edma), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
}

/* Output audio goes to $A8_AUDIO_DIR/<name>.raw when the variable is set. */
static DeviceState *a8_map_mcasp(MachineState *machine, const char *name, hwaddr cfg,
                                 hwaddr dat, DeviceState *edma, int tx_event)
{
    DeviceState *dev = qdev_new(TYPE_A8_MCASP);

    object_property_add_child(OBJECT(machine), name, OBJECT(dev));
    const char *dir = g_getenv("A8_AUDIO_DIR");

    qdev_prop_set_string(dev, "name", name);
    if (dir) {
        g_autofree char *out = g_strdup_printf("%s/%s.raw", dir, name);
        qdev_prop_set_string(dev, "out", out);
    }
    qdev_prop_set_int32(dev, "tx-event", tx_event);
    qdev_prop_set_int32(dev, "rx-event", tx_event + 1);
    object_property_set_link(OBJECT(dev), "edma", OBJECT(edma), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, cfg);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, dat);
    return dev;
}

/* A card is inserted when -drive if=sd,index=<unit> is given. */
static void a8_map_mmc(hwaddr base, qemu_irq irq, DeviceState *edma,
                       int tx_event, int unit)
{
    DeviceState *dev = qdev_new(TYPE_A8_MMC);
    DriveInfo *di = drive_get(IF_SD, 0, unit);
    DeviceState *card;

    qdev_prop_set_int32(dev, "tx-event", tx_event);
    qdev_prop_set_int32(dev, "rx-event", tx_event + 1);
    qdev_prop_set_int32(dev, "unit", unit);
    object_property_set_link(OBJECT(dev), "edma", OBJECT(edma), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    if (di) {
        card = qdev_new(TYPE_SD_CARD);
        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                                &error_fatal);
        qdev_realize_and_unref(card, qdev_get_child_bus(dev, "sd-bus"),
                               &error_fatal);
    }
}

/* "qom-set /machine timescale K": changes A8_TIMESCALE while running (e.g.
 * boot fast, then record audio slowly). Timers pick it up from their next
 * event on. */
static void a8_timescale_get(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    uint32_t k = (uint32_t)a8_timescale;

    visit_type_uint32(v, name, &k, errp);
}

static void a8_timescale_set(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    uint32_t k;

    if (visit_type_uint32(v, name, &k, errp)) {
        a8_timescale = MAX(k, 1);
    }
}

static void a8_init(MachineState *machine)
{
    Object *cpu = object_new(machine->cpu_type);
    DeviceState *intc, *edma;

    qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    object_unref(cpu);

    if (g_getenv("A8_TIMESCALE")) {
        a8_timescale = MAX(g_ascii_strtod(g_getenv("A8_TIMESCALE"), NULL), 1);
    }
    object_property_add(OBJECT(machine), "timescale", "uint32",
                        a8_timescale_get, a8_timescale_set, NULL, NULL);

    memory_region_add_subregion(get_system_memory(), A8_DDR_BASE,
                                machine->ram);

    create_unimplemented_device("unmapped", 0x40000000, 0x40000000);
    for (size_t i = 0; i < ARRAY_SIZE(a8_regions); i++) {
        a8_map_regs(&a8_regions[i].spec, a8_regions[i].base,
                    a8_regions[i].size);
    }
    a8_map_ram("a8.sram-int", 0x402f0000, 0x10000);
    a8_map_ram("a8.ocmc", 0x40300000, 0x10000);

    intc = sysbus_create_simple(TYPE_A8_INTC, 0x48200000, NULL);
    sysbus_connect_irq(SYS_BUS_DEVICE(intc), 0,
                       qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(intc), 1,
                       qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));

    a8_map_dev(TYPE_A8_I2C, "i2c0", 0x44e0b000, qdev_get_gpio_in(intc, 70));
    a8_map_dev(TYPE_A8_I2C, "i2c1", 0x4802a000, qdev_get_gpio_in(intc, 71));
    a8_map_dev(TYPE_A8_I2C, "i2c2", 0x4819c000, qdev_get_gpio_in(intc, 30));
    edma = sysbus_create_simple(TYPE_A8_EDMA, 0x49000000,
                                qdev_get_gpio_in(intc, 12));
    {
        static const struct { hwaddr base; int irq; } timers[] = {
            { 0x44e05000, 66 }, { 0x44e31000, 67 }, { 0x48040000, 68 },
            { 0x48042000, 69 }, { 0x48044000, 92 }, { 0x48046000, 93 },
            { 0x48048000, 94 }, { 0x4804a000, 95 },
        };
        for (size_t i = 0; i < ARRAY_SIZE(timers); i++) {
            sysbus_create_simple(TYPE_A8_TIMER, timers[i].base,
                                 qdev_get_gpio_in(intc, timers[i].irq));
        }
    }
    {
        static const struct {
            const char *name; hwaddr base; int irq; uint32_t inputs;
        } gpios[] = {
            /*
             * Idle levels: panel switches (pull-ups) and the active-low
             * gate inputs read high. GPIO2_25 high = supply good; low for
             * 3 ms makes app shut down. GPIO2_10 high = converter board
             * revision of the module we have (hardware run 10).
             */
            { "gpio0", 0x44e07000, 96, 0xdcc1f403 },
            { "gpio1", 0x4804c000, 98, 0xdffb0000 },
            { "gpio2", 0x481ac000, 32, 0x02100600 },
            { "gpio3", 0x481ae000, 62, 0x0039c010 },
        };
        DeviceState *gpio[ARRAY_SIZE(gpios)];

        for (size_t i = 0; i < ARRAY_SIZE(gpios); i++) {
            DeviceState *dev = qdev_new(TYPE_A8_GPIO);
            gpio[i] = dev;
            object_property_add_child(OBJECT(machine), gpios[i].name,
                                      OBJECT(dev));
            qdev_prop_set_string(dev, "name", gpios[i].name);
            qdev_prop_set_uint32(dev, "inputs", gpios[i].inputs);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, gpios[i].base);
            sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                               qdev_get_gpio_in(intc, gpios[i].irq));
            sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1,
                               qdev_get_gpio_in(intc, gpios[i].irq + 1));
        }
        DeviceState *adc = qdev_new(TYPE_A8_TSCADC);

        object_property_add_child(OBJECT(machine), "adc", OBJECT(adc));
        qdev_prop_set_int32(adc, "fifo0-event", 53);
        object_property_set_link(OBJECT(adc), "edma", OBJECT(edma),
                                 &error_fatal);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(adc), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(adc), 0, 0x44e0d000);
        a8_panel_init(gpio, OBJECT(adc));
    }
    /* OLED on GPMC CS0; app sets CONFIG7 to base 0x01000000, 16 MB. */
    sysbus_create_simple(TYPE_SSD1351, 0x01000000, NULL);
    sysbus_create_simple(TYPE_A8_WDT, 0x44e35000, NULL);
    /* UART0 is the debug header and the Locutus/MIDI link. */
    serial_mm_init(get_system_memory(), 0x44e09000, 2,
                   qdev_get_gpio_in(intc, 72), 48000000 / 16,
                   serial_hd(0), DEVICE_LITTLE_ENDIAN);
    /* McASP1 is clocked from outside at 4x McASP0's frame rate (hw run 14). */
    a8_mcasp_set_clock_slave(
        a8_map_mcasp(machine, "mcasp0", 0x48038000, 0x46000000, edma, 8),
        a8_map_mcasp(machine, "mcasp1", 0x4803c000, 0x46400000, edma, 10), 4);
    a8_map_spi(machine, "mcspi0", 0x48030000, qdev_get_gpio_in(intc, 65),
               edma, 16);
    a8_map_spi(machine, "mcspi1", 0x481a0000, qdev_get_gpio_in(intc, 125),
               edma, 42);
    /* MMC2's events go through the crossbar; app maps them to 2/3. */
    a8_map_mmc(0x48060000, qdev_get_gpio_in(intc, 64), edma, 24, 0);
    a8_map_mmc(0x47810000, qdev_get_gpio_in(intc, 29), edma, 2, 1);
    a8_map_mmc(0x481d8000, qdev_get_gpio_in(intc, 28), edma, 2, 2);
}

static void a8_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a8"),
        NULL
    };

    mc->desc = "Rossum Assimil8or (Olimex AM3352-SOM, Cortex-A8)";
    mc->init = a8_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a8");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = 512 * MiB;
    mc->default_ram_id = "a8.ddr";
    mc->ignore_memory_transaction_failures = true;
}

DEFINE_MACHINE_ARM("assimil8or", a8_machine_init)
