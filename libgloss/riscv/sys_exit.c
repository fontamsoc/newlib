// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

// Exit a program without cleaning up files.
// The ebreak halts the cpu indefinitely (it also ends a simulation), mtvec
// being nulled first; when the gdbstub is linked-in, mtvec is kept valid
// instead, as __halt() does in _os.c, so that the ebreak parks the exiting
// thread awaiting the debugger: the stub stays reachable once the program
// has finished, ie: gdb can still attach, and reload the image. The exit
// status is left in a0, readable by gdb from the parked context.
__attribute__((weak)) void _exit (int status) {
	extern char _gdbstub_active __attribute__((weak));
	register int a0 __asm__("a0") = status;
	if (&_gdbstub_active)
		__asm__ __volatile__ ("ebreak\n" :: "r"(a0) : "memory");
	else
		__asm__ __volatile__ ("csrw mtvec, x0; ebreak\n" :: "r"(a0) : "memory");
	while(1);
}
