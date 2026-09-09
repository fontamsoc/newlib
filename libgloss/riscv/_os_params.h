// SPDX-License-Identifier: GPL-2.0-only
// 20241130 (c) William Fonkou Tambe

#ifndef __LIBGLOSS_RISCV__OS_PARAMS_H
#define __LIBGLOSS_RISCV__OS_PARAMS_H

#define NCPU 16 /* Maximum number of CPUs TODO: To be removed once percpu support is complete. */

#define TRAP_STACK_SHIFT 9
#define TRAP_STACK_SIZE (1 << TRAP_STACK_SHIFT)

// Default clock cycle count it takes to run all threads in a CPU runqueue;
// in other words, a preempted thread is guaranteed to resume in less than
// SCHEDLRHZ clock cycles.
#define SCHEDLRHZ _MSECS(50)

// Whether the trap-return path tail-chains interrupts: before restoring the
// interrupted context, it checks for an interrupt that is already pending and
// enabled, and dispatches it on the still-live saved context, skipping the
// context restore, trap return, hardware re-trap and context re-save that
// back-to-back interrupts otherwise cost.
#define USETAILCHAIN 1

#define SERIAL0_ADDR (0xf80 /* By convention, the first UART is located at 0xf80 */)

// Value which, xored with a hart id, releases that hart from its boot spin
// on __ncpu_rendezvous; its top byte is non-null so that a null word, ie:
// the .bss clear or a load transient starting from zero, matches no hart,
// and its low byte is null so that a byte-wise zeroing of the word matches
// hart 0 only, which never spins. It must be materialized by li and never
// stored as a data word, so that no image carries a hart's target; its
// bits 14:12 being 011, no rv32ima instruction word equals one either.
#define RENDEZVOUS_MAGIC 0x5AFE3A00

#define DFLTDEV_ADDR (-4 /* By convention, the default device fault address register is the last 32bits word of the address space */)
#define DFLTDEV_IRQ (0 /* By convention, the default device is the interrupt source 0 */)

#endif /* __LIBGLOSS_RISCV__OS_PARAMS_H */
