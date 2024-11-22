// SPDX-License-Identifier: GPL-2.0-only
// 20241203 (c) William Fonkou Tambe

// This program is used to generate definitions needed by
// assembly language modules.
//
// We use the technique used in the OSF Mach kernel code:
// generate asm statements containing #defines,
// compile this file to assembler, and then extract the
// #defines from the assembly-language output.

#include <stddef.h>
#include <stdint.h>

#define DEFINE(sym, val) \
	asm volatile("\n.ascii \"->" #sym " %0 " #val "\"" : : "i" (val))

#define OFFSET(sym, typ, mem) \
	DEFINE(sym, offsetof(typ, mem))

#include "_os.h"

void main (void) {
	OFFSET(TRAP_SAVEDCTX_RA,  _trap_savedctx_t, ra);
	OFFSET(TRAP_SAVEDCTX_SP,  _trap_savedctx_t, sp);
	OFFSET(TRAP_SAVEDCTX_T0,  _trap_savedctx_t, t0);
	OFFSET(TRAP_SAVEDCTX_T1,  _trap_savedctx_t, t1);
	OFFSET(TRAP_SAVEDCTX_T2,  _trap_savedctx_t, t2);
	OFFSET(TRAP_SAVEDCTX_S0,  _trap_savedctx_t, s0);
	OFFSET(TRAP_SAVEDCTX_S1,  _trap_savedctx_t, s1);
	OFFSET(TRAP_SAVEDCTX_A0,  _trap_savedctx_t, a0);
	OFFSET(TRAP_SAVEDCTX_A1,  _trap_savedctx_t, a1);
	OFFSET(TRAP_SAVEDCTX_A2,  _trap_savedctx_t, a2);
	OFFSET(TRAP_SAVEDCTX_A3,  _trap_savedctx_t, a3);
	OFFSET(TRAP_SAVEDCTX_A4,  _trap_savedctx_t, a4);
	OFFSET(TRAP_SAVEDCTX_A5,  _trap_savedctx_t, a5);
	OFFSET(TRAP_SAVEDCTX_A6,  _trap_savedctx_t, a6);
	OFFSET(TRAP_SAVEDCTX_A7,  _trap_savedctx_t, a7);
	OFFSET(TRAP_SAVEDCTX_S2,  _trap_savedctx_t, s2);
	OFFSET(TRAP_SAVEDCTX_S3,  _trap_savedctx_t, s3);
	OFFSET(TRAP_SAVEDCTX_S4,  _trap_savedctx_t, s4);
	OFFSET(TRAP_SAVEDCTX_S5,  _trap_savedctx_t, s5);
	OFFSET(TRAP_SAVEDCTX_S6,  _trap_savedctx_t, s6);
	OFFSET(TRAP_SAVEDCTX_S7,  _trap_savedctx_t, s7);
	OFFSET(TRAP_SAVEDCTX_S8,  _trap_savedctx_t, s8);
	OFFSET(TRAP_SAVEDCTX_S9,  _trap_savedctx_t, s9);
	OFFSET(TRAP_SAVEDCTX_S10, _trap_savedctx_t, s10);
	OFFSET(TRAP_SAVEDCTX_S11, _trap_savedctx_t, s11);
	OFFSET(TRAP_SAVEDCTX_T3,  _trap_savedctx_t, t3);
	OFFSET(TRAP_SAVEDCTX_T4,  _trap_savedctx_t, t4);
	OFFSET(TRAP_SAVEDCTX_T5,  _trap_savedctx_t, t5);
	OFFSET(TRAP_SAVEDCTX_T6,  _trap_savedctx_t, t6);

	OFFSET(TRAP_SAVEDCTX_SCRATCH, _trap_savedctx_t, scratch);
	OFFSET(TRAP_SAVEDCTX_STATUS,  _trap_savedctx_t, status);
	OFFSET(TRAP_SAVEDCTX_EPC,     _trap_savedctx_t, epc);
	OFFSET(TRAP_SAVEDCTX_TVAL,    _trap_savedctx_t, tval);
	OFFSET(TRAP_SAVEDCTX_TVAL2,   _trap_savedctx_t, tval2);
	OFFSET(TRAP_SAVEDCTX_CAUSE,   _trap_savedctx_t, cause);
	OFFSET(TRAP_SAVEDCTX_CYCLE,   _trap_savedctx_t, cycle);
#if __riscv_xlen == 32
	DEFINE(TRAP_SAVEDCTX_CYCLEH, (offsetof(_trap_savedctx_t, cycle) + 4));
#endif

	DEFINE(TRAP_SAVEDCTX_SIZE, sizeof(_trap_savedctx_t));

	OFFSET(THREAD_CPU, _thread_t, cpu);

	OFFSET(THREAD_SAVEDCTX_RA,  _thread_t, savedctx.ra);
	OFFSET(THREAD_SAVEDCTX_SP,  _thread_t, savedctx.sp);
	OFFSET(THREAD_SAVEDCTX_S0,  _thread_t, savedctx.s0);
	OFFSET(THREAD_SAVEDCTX_S1,  _thread_t, savedctx.s1);
	OFFSET(THREAD_SAVEDCTX_S2,  _thread_t, savedctx.s2);
	OFFSET(THREAD_SAVEDCTX_S3,  _thread_t, savedctx.s3);
	OFFSET(THREAD_SAVEDCTX_S4,  _thread_t, savedctx.s4);
	OFFSET(THREAD_SAVEDCTX_S5,  _thread_t, savedctx.s5);
	OFFSET(THREAD_SAVEDCTX_S6,  _thread_t, savedctx.s6);
	OFFSET(THREAD_SAVEDCTX_S7,  _thread_t, savedctx.s7);
	OFFSET(THREAD_SAVEDCTX_S8,  _thread_t, savedctx.s8);
	OFFSET(THREAD_SAVEDCTX_S9,  _thread_t, savedctx.s9);
	OFFSET(THREAD_SAVEDCTX_S10, _thread_t, savedctx.s10);
	OFFSET(THREAD_SAVEDCTX_S11, _thread_t, savedctx.s11);

	OFFSET(THREAD_SAVEDCTX_SCRATCH, _thread_t, savedctx.scratch);
	OFFSET(THREAD_SAVEDCTX_STATUS,  _thread_t, savedctx.status);

	DEFINE(THREAD_STRUCT_SIZE, sizeof(_thread_t));
}
