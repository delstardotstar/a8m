#ifndef INTC_H_INCLUDED
#define INTC_H_INCLUDED

#include <stdint.h>

/* AM335x interrupt numbers used here. */
#define IRQ_EDMA_COMPLETION 12
#define IRQ_TIMER3          69
#define IRQ_GPIO0A          96

/* Where the CPU was when the current interrupt was taken. */
extern uint32_t irq_interrupted_pc;

typedef void (*irq_fn)(void);

void intc_init(void);
void intc_enable(unsigned irq, irq_fn fn);
/* Higher priority: may preempt a handler inside intc_nest(). */
void intc_enable_urgent(unsigned irq, irq_fn fn);
/* From an IRQ handler: runs fn with urgent interrupts enabled. */
void intc_nest(irq_fn fn);
void intc_enable_fiq(unsigned irq);
/* End of an FIQ handler: lets the controller raise the next one. */
void intc_fiq_done(void);

static inline void irq_on(void)
{
    __asm__ volatile("cpsie i" ::: "memory");
}

#endif
