// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Open file relative to given directory.
__attribute__((weak)) int _openat (int dirfd, const char *name, int flags, int mode) {
	errno = EPERM;
	return -1;
}
