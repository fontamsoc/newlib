// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Open a file.
__attribute__((weak)) int _open (const char *path, int flags, int mode) {
	errno = EPERM;
	return -1;
}
