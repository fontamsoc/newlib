// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Permissions of a file (by name) in a given directory.
__attribute__((weak)) int _faccessat (int dirfd, const char *path, int mode, int flags) {
	errno = EPERM;
	return -1;
}
