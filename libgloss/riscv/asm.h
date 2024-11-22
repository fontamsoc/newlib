// SPDX-License-Identifier: GPL-2.0-only
// 20241130 (c) William Fonkou Tambe

#ifndef __LIBGLOSS_RISCV_ASM_H
#define __LIBGLOSS_RISCV_ASM_H

#if __riscv_xlen == 64
#define __XLEN_SEL(a, b) a
#elif __riscv_xlen == 32
#define __XLEN_SEL(a, b) b
#else
#error "Unexpected __riscv_xlen"
#endif

#define REG_L __XLEN_SEL(ld, lw)
#define REG_S __XLEN_SEL(sd, sw)
#define LITWORD __XLEN_SEL(.dword, .word)
#define SZREG __XLEN_SEL(8, 4)
#define LGREG __XLEN_SEL(3, 2)

#endif /* __LIBGLOSS_RISCV_ASM_H */
