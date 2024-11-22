// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Permissions of a file (by name).
__attribute__((weak)) int _access (const char *path, int mode) {
	errno = EPERM;
	return -1;
}
