// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

// Exit a program without cleaning up files.
__attribute__((weak)) void _exit (int status) {
	__asm__ __volatile__ ("csrw mtvec, x0; ebreak\n" ::: "memory");
	while(1);
}
