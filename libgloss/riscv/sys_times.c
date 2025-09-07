// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/times.h>
#include <errno.h>

// Timing information for current process.
__attribute__((weak)) clock_t _times(struct tms *buf) {
	errno = EPERM;
	return -1;
}
