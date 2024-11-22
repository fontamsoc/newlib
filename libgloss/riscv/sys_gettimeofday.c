// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/time.h>
#include <errno.h>

// Get the current time.  Only relatively correct.
__attribute__((weak)) int _gettimeofday (struct timeval *tp, void *tzp) {
	errno = EPERM;
	return -1;
}
