/* SPDX-License-Identifier: GPL-2.0-only
   20241203 (c) William Fonkou Tambe */

#include "newlib.h"

#include "asm.h"
#include "asm-offsets.h"

#include "_os_params.h"


  .section ".text._start"
  .global  _start
  .type    _start, @function
_start:

  # Register sp is assumed set to where memory ends.

  # Init global pointer.
  .option push
  .option norelax
0:auipc gp, %pcrel_hi(__global_pointer$)
  addi  gp, gp, %pcrel_lo(0b)
  .option pop

  la   t0, __trap
  csrw mtvec, t0
  csrc mstatus, 0x8 /* Clear mstatus.mie to disable IRQs */
  li   t0, 0x800 # Only set mie.meie to receive external IRQs.
  csrw mie, t0
  csrw mscratch, x0
  /* CPU initialization is done above, while program
     initialization is done below only by one cpu */
  csrr s0, mhartid
  beqz s0, 1f
  # For an SOC where all cores start, loop here until (__ncpu == mhartid).
  la    s1, __ncpu
0:REG_L t0, 0(s1)
  bne   s0, t0, 0b
  # Use temporary trap stack right below the stack currently used by CPU 0.
  la    sp, __heap_end
  REG_L sp, 0(sp)
  addi  sp, sp, -TRAP_STACK_SIZE
  mv tp, x0
  call __init_secondary_cpu
  # Increment __ncpu. Saved register s1 has __ncpu address.
  REG_L t0, 0(s1)
  addi  t0, t0, 1
  REG_S t0, 0(s1)
  # Compute trap stack. Saved register s0 has mhartid value. TODO: Only after percpu data has been setup ...
  slli s0, s0, TRAP_STACK_SHIFT
  addi s0, s0, TRAP_STACK_SIZE
  la sp, __trap_stacks # TODO: Get __trap_stacks calling __percpu_ptr();
  add sp, sp, s0
  csrs mstatus, 0x8 /* Set mstatus.mie to enable IRQs */
  wfi
1:
  REG_L a2, __rst_flg
  bnez  a2, 0f
  # Save .data section to the stack bottom.
  la    a1, __DATA_BEGIN__
  la    a2, _edata
  sub   a2, a2, a1
  sub   sp, sp, a2
  mv    a0, sp
  REG_S a2, __rst_flg, t0
  call  memcpy
  j 1f
0:# Restore .data section from the stack bottom.
  la    a0, __DATA_BEGIN__
  sub   sp, sp, a2
  mv    a1, sp
  call  memcpy
1:
  # Init BSS.
  la   a0, __bss_start
  la   a2, _end
  sub  a2, a2, a0
  li   a1, 0
  call memset

  # Stack layout:
  # ------------------------ Start of stack.
  # | Stack Data           |
  # ------------------------ <- Stack pointer when calling main().
  # | TLS Data             |
  # | _thread_cur is first |
  # ------------------------ End of stack.

  # Init TLS.
  la   a0, __tbss_start
  la   a2, __tbss_end
  sub  a2, a2, a0
  sub  sp, sp, a2
  mv   a0, sp
  li   a1, 0
  call memset
  la    a1, __tdata_start
  la    a2, __tdata_end
  sub   a2, a2, a1
  sub   sp, sp, a2
  mv    a0, sp
  call  memcpy
  addi  sp, sp, -THREAD_STRUCT_SIZE
  mv    tp, sp

  # Init __heap_ptr to _end, and __heap_end to the value of register sp.
  la    t0, __heap_ptr
  la    t1, _end
  REG_S t1, 0(t0)
  la    t0, __heap_end
  REG_S sp, 0(t0)

  mv a0, tp
  call __init_multithreading

#ifdef _LITE_EXIT
  # Make reference to atexit weak to avoid unconditionally pulling in
  # support code.  Refer to comments in __atexit.c for more details.
  .weak atexit
  la    a0, atexit
  beqz  a0, .Lweak_atexit
  .weak __libc_fini_array
#endif

  la   a0, __libc_fini_array   # Register global termination functions
  call atexit                  #  to be called upon exit
#ifdef _LITE_EXIT
.Lweak_atexit:
#endif
  call __libc_init_array       # Run global initialization functions

  # argc, argv, envp are all set to null.
  li a0, 0
  li a1, 0
  li a2, 0

  call  main
  tail  exit

  .size _start, .-_start


  .text
  .global __trap
  .type   __trap, @function
__trap:

  addi  sp, sp, -TRAP_SAVEDCTX_SIZE

  beqz tp, 0f

  REG_S ra, TRAP_SAVEDCTX_RA(sp)
  /* sp is saved later */
  REG_S t0, TRAP_SAVEDCTX_T0(sp)
  REG_S t1, TRAP_SAVEDCTX_T1(sp)
  REG_S t2, TRAP_SAVEDCTX_T2(sp)
  REG_S s0, TRAP_SAVEDCTX_S0(sp)
  REG_S s1, TRAP_SAVEDCTX_S1(sp)
  REG_S a0, TRAP_SAVEDCTX_A0(sp)
  REG_S a1, TRAP_SAVEDCTX_A1(sp)
  REG_S a2, TRAP_SAVEDCTX_A2(sp)
  REG_S a3, TRAP_SAVEDCTX_A3(sp)
  REG_S a4, TRAP_SAVEDCTX_A4(sp)
  REG_S a5, TRAP_SAVEDCTX_A5(sp)
  REG_S a6, TRAP_SAVEDCTX_A6(sp)
  REG_S a7, TRAP_SAVEDCTX_A7(sp)
  REG_S s2, TRAP_SAVEDCTX_S2(sp)
  REG_S s3, TRAP_SAVEDCTX_S3(sp)
  REG_S s4, TRAP_SAVEDCTX_S4(sp)
  REG_S s5, TRAP_SAVEDCTX_S5(sp)
  REG_S s6, TRAP_SAVEDCTX_S6(sp)
  REG_S s7, TRAP_SAVEDCTX_S7(sp)
  REG_S s8, TRAP_SAVEDCTX_S8(sp)
  REG_S s9, TRAP_SAVEDCTX_S9(sp)
  REG_S s10, TRAP_SAVEDCTX_S10(sp)
  REG_S s11, TRAP_SAVEDCTX_S11(sp)
  REG_S t3, TRAP_SAVEDCTX_T3(sp)
  REG_S t4, TRAP_SAVEDCTX_T4(sp)
  REG_S t5, TRAP_SAVEDCTX_T5(sp)
  REG_S t6, TRAP_SAVEDCTX_T6(sp)

  addi  t0, sp, TRAP_SAVEDCTX_SIZE
  REG_S t0, TRAP_SAVEDCTX_SP(sp)
0:
  csrr  t1, mscratch
  csrr  t2, mstatus
  csrr  t3, mepc
  csrr  t4, mtval
  csrr  t5, mtval2
  csrr  t6, mcause
  REG_S t1, TRAP_SAVEDCTX_SCRATCH(sp)
  REG_S t2, TRAP_SAVEDCTX_STATUS(sp)
  REG_S t3, TRAP_SAVEDCTX_EPC(sp)
  REG_S t4, TRAP_SAVEDCTX_TVAL(sp)
  REG_S t5, TRAP_SAVEDCTX_TVAL2(sp)
  REG_S t6, TRAP_SAVEDCTX_CAUSE(sp)

  call _clkcycles
  REG_S a0, TRAP_SAVEDCTX_CYCLE(sp)
#if __riscv_xlen == 32
  REG_S a1, TRAP_SAVEDCTX_CYCLEH(sp)
#endif

  csrw mscratch, sp /* _trap_savedctx */

  # MSB of cause differentiates between
  # interrupts and exceptions.
  bge t6, x0, 0f

  # Handle interrupts.
  call __trap_irq
  j 1f
0: # Handle other exceptions.
  slli t0, t6, LGREG
  la t1, .Lexc_tbl_start
  la t2, .Lexc_tbl_end
  add t0, t1, t0
  bgeu t0, t2, 0f
  REG_L t0, 0(t0)
  jalr t0
  j 1f
0:
  call __trap_exc_inv
__trap_ret: # Label used from __switchctx().
1: # Skip to next instruction if true was returned.
  REG_L t3, TRAP_SAVEDCTX_EPC(sp)
  beqz a0, 0f
  addi t3, t3, 4
0:
  csrw mepc, t3
  REG_L t2, TRAP_SAVEDCTX_STATUS(sp)
  csrw mstatus, t2
  REG_L t1, TRAP_SAVEDCTX_SCRATCH(sp)
  csrw mscratch, t1

  beqz tp, 0f

  REG_L ra, TRAP_SAVEDCTX_RA(sp)
  /* sp is restored later */
  REG_L t0, TRAP_SAVEDCTX_T0(sp)
  REG_L t1, TRAP_SAVEDCTX_T1(sp)
  REG_L t2, TRAP_SAVEDCTX_T2(sp)
  REG_L s0, TRAP_SAVEDCTX_S0(sp)
  REG_L s1, TRAP_SAVEDCTX_S1(sp)
  REG_L a0, TRAP_SAVEDCTX_A0(sp)
  REG_L a1, TRAP_SAVEDCTX_A1(sp)
  REG_L a2, TRAP_SAVEDCTX_A2(sp)
  REG_L a3, TRAP_SAVEDCTX_A3(sp)
  REG_L a4, TRAP_SAVEDCTX_A4(sp)
  REG_L a5, TRAP_SAVEDCTX_A5(sp)
  REG_L a6, TRAP_SAVEDCTX_A6(sp)
  REG_L a7, TRAP_SAVEDCTX_A7(sp)
  REG_L s2, TRAP_SAVEDCTX_S2(sp)
  REG_L s3, TRAP_SAVEDCTX_S3(sp)
  REG_L s4, TRAP_SAVEDCTX_S4(sp)
  REG_L s5, TRAP_SAVEDCTX_S5(sp)
  REG_L s6, TRAP_SAVEDCTX_S6(sp)
  REG_L s7, TRAP_SAVEDCTX_S7(sp)
  REG_L s8, TRAP_SAVEDCTX_S8(sp)
  REG_L s9, TRAP_SAVEDCTX_S9(sp)
  REG_L s10, TRAP_SAVEDCTX_S10(sp)
  REG_L s11, TRAP_SAVEDCTX_S11(sp)
  REG_L t3, TRAP_SAVEDCTX_T3(sp)
  REG_L t4, TRAP_SAVEDCTX_T4(sp)
  REG_L t5, TRAP_SAVEDCTX_T5(sp)
  REG_L t6, TRAP_SAVEDCTX_T6(sp)
  REG_L sp, TRAP_SAVEDCTX_SP(sp)
  mret
0:
  addi sp, sp, TRAP_SAVEDCTX_SIZE
  csrs mstatus, 0x8 /* Set mstatus.mie to enable IRQs */
  wfi

  .size __trap, .-__trap


  .text
  .global __switchctx
  .type   __switchctx, @function
__switchctx:

  /* void __switchctx (_thread_t *to) */
  /* WARNING: Must be called with IRQs disabled */

  beqz tp, 0f

  csrr  t1, mscratch
  REG_S t1, THREAD_SAVEDCTX_SCRATCH(tp)
  /* If mscratch is non-null, skip saving the other THREAD_SAVEDCTX_*
     as the thread will be restored from its trap context. */
  bnez t1, 0f

  REG_S ra, THREAD_SAVEDCTX_RA(tp)
  REG_S sp, THREAD_SAVEDCTX_SP(tp)
  REG_S s0, THREAD_SAVEDCTX_S0(tp)
  REG_S s1, THREAD_SAVEDCTX_S1(tp)
  REG_S s2, THREAD_SAVEDCTX_S2(tp)
  REG_S s3, THREAD_SAVEDCTX_S3(tp)
  REG_S s4, THREAD_SAVEDCTX_S4(tp)
  REG_S s5, THREAD_SAVEDCTX_S5(tp)
  REG_S s6, THREAD_SAVEDCTX_S6(tp)
  REG_S s7, THREAD_SAVEDCTX_S7(tp)
  REG_S s8, THREAD_SAVEDCTX_S8(tp)
  REG_S s9, THREAD_SAVEDCTX_S9(tp)
  REG_S s10, THREAD_SAVEDCTX_S10(tp)
  REG_S s11, THREAD_SAVEDCTX_S11(tp)

  csrr  t2, mstatus
  REG_S t2, THREAD_SAVEDCTX_STATUS(tp)
0:
  bnez a0, 0f
  csrr t0, mhartid
  slli t0, t0, TRAP_STACK_SHIFT
  addi t0, t0, TRAP_STACK_SIZE
  la sp, __trap_stacks
  add sp, sp, t0
  mv tp, x0
  csrs mstatus, 0x8 /* Set mstatus.mie to enable IRQs */
  wfi
0:
  mv tp, a0

  REG_L t1, THREAD_SAVEDCTX_SCRATCH(tp)
  /* If the thread mscratch is non-null, restore from trap context */
  beqz t1, 0f
  mv sp, t1
  mv a0, x0
  j __trap_ret
0:
  REG_L t2, THREAD_SAVEDCTX_STATUS(tp)

  REG_L ra, THREAD_SAVEDCTX_RA(tp)
  REG_L sp, THREAD_SAVEDCTX_SP(tp)

  csrw mscratch, t1
  csrw mstatus, t2

  REG_L t3, THREAD_CPU(tp)
  /* If the thread cpu number is negative, restore
     s0 in a0, and skip restoring saved registers */
  bge t3, x0, 0f
  neg t3, t3
  addi t3, t3, -1
  REG_S t3, THREAD_CPU(tp)
  REG_L t4, THREAD_SAVEDCTX_S1(tp)
  REG_L a0, THREAD_SAVEDCTX_S0(tp)
  jr t4
0:
  REG_L s0, THREAD_SAVEDCTX_S0(tp)
  REG_L s1, THREAD_SAVEDCTX_S1(tp)
  REG_L s2, THREAD_SAVEDCTX_S2(tp)
  REG_L s3, THREAD_SAVEDCTX_S3(tp)
  REG_L s4, THREAD_SAVEDCTX_S4(tp)
  REG_L s5, THREAD_SAVEDCTX_S5(tp)
  REG_L s6, THREAD_SAVEDCTX_S6(tp)
  REG_L s7, THREAD_SAVEDCTX_S7(tp)
  REG_L s8, THREAD_SAVEDCTX_S8(tp)
  REG_L s9, THREAD_SAVEDCTX_S9(tp)
  REG_L s10, THREAD_SAVEDCTX_S10(tp)
  REG_L s11, THREAD_SAVEDCTX_S11(tp)
  ret

  .size __switchctx, .-__switchctx


  .section ".rodata"
  .p2align LGREG
.Lexc_tbl_start: # Exception table.
  LITWORD __trap_exc_insn_misaligned
  LITWORD __trap_exc_insn_afault
  LITWORD __trap_exc_insn_illegal
  LITWORD __trap_exc_break
  LITWORD __trap_exc_load_misaligned
  LITWORD __trap_exc_load_afault
  LITWORD __trap_exc_store_misaligned
  LITWORD __trap_exc_store_afault
  LITWORD __trap_exc_ecall_u
  LITWORD __trap_exc_ecall_s
  LITWORD __trap_exc_inv # Reserved.
  LITWORD __trap_exc_ecall_m
  LITWORD __trap_exc_insn_pfault
  LITWORD __trap_exc_load_pfault
  LITWORD __trap_exc_inv # Reserved.
  LITWORD __trap_exc_store_pfault
.Lexc_tbl_end:


  .data
  .p2align LGREG
  .global __rst_flg;  __rst_flg:  LITWORD 0
  .global __heap_ptr; __heap_ptr: LITWORD 0
  .global __heap_end; __heap_end: LITWORD 0
  .size   __rst_flg,  SZREG
  .size   __heap_ptr, SZREG
  .size   __heap_end, SZREG


  .section ".tdata._thread_cur"
  .p2align LGREG
  .global _thread_cur; _thread_cur: .skip THREAD_STRUCT_SIZE
  .size   _thread_cur, THREAD_STRUCT_SIZE
