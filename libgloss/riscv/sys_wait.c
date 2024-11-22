// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Wait for a child process. Minimal implementation
// for a system without processes just causes an error.
__attribute__((weak)) int _wait (int *status) {
	errno = ECHILD;
	return -1;
}
