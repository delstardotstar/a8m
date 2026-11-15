#include "intc.h"
#include "hw.h"

#define INTC            0x48200000u
#define INTC_SYSCONFIG  0x010u
#define INTC_SYSSTATUS  0x014u
#define INTC_SIR_IRQ    0x040u
#define INTC_CONTROL    0x048u
#define INTC_THRESHOLD  0x068u
#define CONTROL_NEWFIQAGR 0x2u
#define INTC_MIR_CLEAR(bank) (0x088u + 0x20u * (bank))
#define INTC_ILR(n)     (0x100u + 4u * (n))
#define SYSCONFIG_SOFTRESET 0x2u
#define CONTROL_NEWIRQAGR 0x1u
#define SIR_ACTIVE_MASK 0x7fu
#define NIRQ            128
#define PRIO_URGENT     0u
#define PRIO_NORMAL     1u

static irq_fn handlers[NIRQ];

void intc_init(void)
{
    REG(INTC + INTC_SYSCONFIG) = SYSCONFIG_SOFTRESET;
    while (!(REG(INTC + INTC_SYSSTATUS) & 1u)) {
    }
}

static void enable(unsigned irq, irq_fn fn, uint32_t prio)
{
    handlers[irq] = fn;
    REG(INTC + INTC_ILR(irq)) = prio << 2;
    REG(INTC + INTC_MIR_CLEAR(irq / 32)) = 1u << (irq % 32);
}

void intc_enable(unsigned irq, irq_fn fn)
{
    enable(irq, fn, PRIO_NORMAL);
}

void intc_enable_urgent(unsigned irq, irq_fn fn)
{
    enable(irq, fn, PRIO_URGENT);
}

void irq_nest_call(irq_fn fn);

void intc_nest(irq_fn fn)
{
    uint32_t pc = irq_interrupted_pc;

    REG(INTC + INTC_THRESHOLD) = PRIO_NORMAL;
    REG(INTC + INTC_CONTROL) = CONTROL_NEWIRQAGR;
    __asm__ volatile("dsb" ::: "memory");
    irq_nest_call(fn);
    REG(INTC + INTC_THRESHOLD) = 0xffu;
    irq_interrupted_pc = pc;
}

/* Routed to FIQ (ILR FIQNIRQ); the handler is the FIQ vector's. */
void intc_enable_fiq(unsigned irq)
{
    REG(INTC + INTC_ILR(irq)) = 1u;
    REG(INTC + INTC_MIR_CLEAR(irq / 32)) = 1u << (irq % 32);
}

void intc_fiq_done(void)
{
    REG(INTC + INTC_CONTROL) = CONTROL_NEWFIQAGR;
    __asm__ volatile("dsb" ::: "memory");
}

uint32_t irq_interrupted_pc;

/* Called from start.S with the interrupted context saved. */
void irq_handler(uint32_t pc)
{
    irq_interrupted_pc = pc;
    unsigned irq = REG(INTC + INTC_SIR_IRQ) & SIR_ACTIVE_MASK;

    if (irq < NIRQ && handlers[irq]) {
        handlers[irq]();
    }
    REG(INTC + INTC_CONTROL) = CONTROL_NEWIRQAGR;
    __asm__ volatile("dsb" ::: "memory");
}
