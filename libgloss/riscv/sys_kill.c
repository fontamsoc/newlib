// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Send a signal. Minimal implementation for a
// system without processes just causes an error.
__attribute__((weak)) int _kill (int pid, int sig) {
	errno = EINVAL;
	return -1;
}
