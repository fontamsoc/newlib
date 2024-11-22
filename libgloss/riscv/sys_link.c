// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Establish a new name for an existing file.
__attribute__((weak)) int _link (const char *oldpath, const char *newpath) {
	errno = EPERM;
	return -1;
}
