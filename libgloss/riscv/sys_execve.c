// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Transfer control to a new process. Minimal implementation
// for a system without processes from newlib documentation.
__attribute__((weak)) int _execve (const char *path, char *const argv[], char *const env[]) {
	errno = ENOMEM;
	return -1;
}
