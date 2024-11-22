// SPDX-License-Identifier: GPL-2.0-only
// 20260504 (c) William Fonkou Tambe

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
	OFFSET(SAVEDCTX_RA,  _savedctx_t, ra);
	OFFSET(SAVEDCTX_SP,  _savedctx_t, sp);
	OFFSET(SAVEDCTX_T0,  _savedctx_t, t0);
	OFFSET(SAVEDCTX_T1,  _savedctx_t, t1);
	OFFSET(SAVEDCTX_T2,  _savedctx_t, t2);
	OFFSET(SAVEDCTX_S0,  _savedctx_t, s0);
	OFFSET(SAVEDCTX_S1,  _savedctx_t, s1);
	OFFSET(SAVEDCTX_A0,  _savedctx_t, a0);
	OFFSET(SAVEDCTX_A1,  _savedctx_t, a1);
	OFFSET(SAVEDCTX_A2,  _savedctx_t, a2);
	OFFSET(SAVEDCTX_A3,  _savedctx_t, a3);
	OFFSET(SAVEDCTX_A4,  _savedctx_t, a4);
	OFFSET(SAVEDCTX_A5,  _savedctx_t, a5);
	OFFSET(SAVEDCTX_A6,  _savedctx_t, a6);
	OFFSET(SAVEDCTX_A7,  _savedctx_t, a7);
	OFFSET(SAVEDCTX_S2,  _savedctx_t, s2);
	OFFSET(SAVEDCTX_S3,  _savedctx_t, s3);
	OFFSET(SAVEDCTX_S4,  _savedctx_t, s4);
	OFFSET(SAVEDCTX_S5,  _savedctx_t, s5);
	OFFSET(SAVEDCTX_S6,  _savedctx_t, s6);
	OFFSET(SAVEDCTX_S7,  _savedctx_t, s7);
	OFFSET(SAVEDCTX_S8,  _savedctx_t, s8);
	OFFSET(SAVEDCTX_S9,  _savedctx_t, s9);
	OFFSET(SAVEDCTX_S10, _savedctx_t, s10);
	OFFSET(SAVEDCTX_S11, _savedctx_t, s11);
	OFFSET(SAVEDCTX_T3,  _savedctx_t, t3);
	OFFSET(SAVEDCTX_T4,  _savedctx_t, t4);
	OFFSET(SAVEDCTX_T5,  _savedctx_t, t5);
	OFFSET(SAVEDCTX_T6,  _savedctx_t, t6);

	OFFSET(SAVEDCTX_SCRATCH, _savedctx_t, scratch);
	OFFSET(SAVEDCTX_STATUS,  _savedctx_t, status);
	OFFSET(SAVEDCTX_EPC,     _savedctx_t, epc);
	OFFSET(SAVEDCTX_TVAL,    _savedctx_t, tval);
	OFFSET(SAVEDCTX_TVAL2,   _savedctx_t, tval2);
	OFFSET(SAVEDCTX_CAUSE,   _savedctx_t, cause);

	DEFINE(SAVEDCTX_SIZE, sizeof(_savedctx_t));

	OFFSET(THREAD_CPU,      _thread_t, cpu);
	OFFSET(THREAD_CTXSAVED, _thread_t, ctxsaved);
	OFFSET(THREAD_SAVEDCTX, _thread_t, savedctx);

	DEFINE(THREAD_STRUCT_SIZE, sizeof(_thread_t));
}
