/* Entry from Rossum's MLO, which copies the image to 0x80000000 and jumps
 * to its first word: SVC mode, MMU and caches off.
 *
 * Layout as the stock app's: 8 vectors loading handler addresses from the
 * table at 0x20-0x3f. MLO writes a word (1 on hardware run 8) at 0x34, the
 * reserved vector's slot, so nothing but that slot may live there. */
    .syntax unified
    .arm
    .section .vectors, "ax"
    .global _start
_start:
    ldr pc, vec_reset
    ldr pc, vec_undef
    ldr pc, vec_svc
    ldr pc, vec_pabort
    ldr pc, vec_dabort
    ldr pc, vec_reserved
    ldr pc, vec_irq
    ldr pc, vec_fiq
vec_reset:      .word reset
vec_undef:      .word fault_undef
vec_svc:        .word fault_svc
vec_pabort:     .word fault_pabort
vec_dabort:     .word fault_dabort
    .global mlo_boot_word
vec_reserved:
mlo_boot_word:  .word 0     /* written by MLO; never used as a vector */
vec_irq:        .word irq_entry
vec_fiq:        .word fiq_entry

    .text
reset:
    cpsid if
    /* As stock: drop the branch predictor and instruction cache contents
     * MLO left behind, since our code was just written to memory. */
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0       /* ICIALLU */
    mcr p15, 0, r0, c7, c5, 6       /* BPIALL */
    dsb
    isb
    ldr r0, =_start
    mcr p15, 0, r0, c12, c0, 0      /* VBAR */

    /* FIQ, IRQ, then SVC stack. */
    cps #0x11
    ldr sp, =fiq_stack_top
    cps #0x12
    ldr sp, =__irq_stack_top
    cps #0x13
    ldr sp, =__svc_stack_top

    /* VFP/NEON: CPACR full access to cp10/cp11, then FPEXC.EN. */
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #(0xf << 20)
    mcr p15, 0, r0, c1, c0, 2
    isb
    mov r0, #0x40000000
    vmsr fpexc, r0

    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b

    bl main
hang:
    b hang

/* CPU exception: report it on the OLED (diag.c) on a stack of its own (the
 * abort and undefined modes' banked sp is never set). diag_fault gets the
 * kind, r0-r12 + lr as saved here, the SPSR, and the faulting mode's own
 * sp and lr (read by switching to that mode; r0/r1 are not banked). */
fault_undef:
    cpsid f
    ldr sp, =fault_stack_top
    push {r0-r12, lr}
    mov r4, #1
    b fault_common
fault_svc:
    cpsid f
    ldr sp, =fault_stack_top
    push {r0-r12, lr}
    mov r4, #2
    b fault_common
fault_pabort:
    cpsid f
    ldr sp, =fault_stack_top
    push {r0-r12, lr}
    mov r4, #3
    b fault_common
fault_dabort:
    cpsid f
    ldr sp, =fault_stack_top
    push {r0-r12, lr}
    mov r4, #4
fault_common:
    mov r5, sp
    mrs r6, spsr
    bl banked_sp_lr
    push {r0, r1}
    mov r3, sp
    mov r0, r4
    mov r1, r5
    mov r2, r6
    bl diag_fault
1:  b 1b

/* r6 = an SPSR: returns that mode's sp in r0 and lr in r1 (user -> system).
 * Clobbers r2, r3 only. */
banked_sp_lr:
    and r2, r6, #0x1f
    cmp r2, #0x10
    moveq r2, #0x1f
    orr r2, r2, #0xc0
    mrs r3, cpsr
    msr cpsr_c, r2
    mov r0, sp
    mov r1, lr
    msr cpsr_c, r3
    bx lr

    .bss
    .balign 8
    .space 4096
fault_stack_top:
    .space 2048
fiq_stack_top:
    .space 2048
nest_stack_top:
    .text

/* FIQ: the watchdog timer (diag.c). diag_fiq gets the saved frame: r0-r7,
 * the interrupted PC, its CPSR, and its mode's sp and lr. Only r0-r7 are
 * shared with other modes; VFP state is saved as in irq_entry. */
fiq_entry:
    sub lr, lr, #4
    push {r0-r7, lr}
    mrs r6, spsr
    bl banked_sp_lr
    push {r0, r1, r6}
    vmrs r0, fpscr
    push {r0, r1}
    vpush {d0-d7}
    vpush {d16-d31}
    add r0, sp, #(8 + 16 * 8 + 8 * 8)
    bl diag_fiq
    vpop {d16-d31}
    vpop {d0-d7}
    pop {r0, r1}
    vmsr fpscr, r0
    add sp, sp, #12
    ldm sp!, {r0-r7, pc}^

/* r0 = fn, from an IRQ handler (IRQ mode, IRQs masked): fn runs in system
 * mode on this stack with IRQs on; interrupts nesting meanwhile use
 * nest_stack (sp_irq), and clobber only lr_irq/spsr_irq, saved here. */
    .global irq_nest_call
irq_nest_call:
    push {r4-r6, lr}
    mrs r4, spsr
    mov r5, sp
    ldr sp, =nest_stack_top
    cps #0x1f
    mov r6, sp
    mov sp, r5
    push {r6, lr}
    cpsie i
    blx r0
    cpsid i
    pop {r6, lr}
    mov sp, r6
    cps #0x12
    mov sp, r5
    msr spsr_cxsf, r4
    pop {r4-r6, pc}

irq_entry:
    sub lr, lr, #4
    push {r0-r3, r12, lr}
    vmrs r0, fpscr
    push {r0, r1}                   /* r1 pads: C needs an 8-byte aligned sp */
    vpush {d0-d7}
    vpush {d16-d31}
    ldr r0, [sp, #(8 + 16 * 8 + 8 * 8 + 5 * 4)]   /* interrupted PC (saved lr) */
    bl irq_handler
    vpop {d16-d31}
    vpop {d0-d7}
    pop {r0, r1}
    vmsr fpscr, r0
    ldm sp!, {r0-r3, r12, pc}^
