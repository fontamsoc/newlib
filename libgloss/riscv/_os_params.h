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

#endif /* __LIBGLOSS_RISCV__OS_PARAMS_H */
