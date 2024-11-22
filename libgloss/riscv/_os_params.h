// SPDX-License-Identifier: GPL-2.0-only
// 20241130 (c) William Fonkou Tambe

#ifndef __LIBGLOSS_RISCV__OS_PARAMS_H
#define __LIBGLOSS_RISCV__OS_PARAMS_H

#define NCPU 16 /* Maximum number of CPUs TODO: To be removed once percpu support is complete. */

#define TRAP_STACK_SHIFT 9
#define TRAP_STACK_SIZE (1 << TRAP_STACK_SHIFT)

// Clock cycles it takes to schedule preempt all threads in a CPU.
// Can be modified at runtime per cpu using _schedlr_freq().
#define SCHEDLRHZ _MSECS(100)

#define SERIAL0_ADDR (0xf80 /* By convention, the first UART is located at 0xf80 */)

#endif /* __LIBGLOSS_RISCV__OS_PARAMS_H */
