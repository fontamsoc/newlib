// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Remove a file's directory entry.
__attribute__((weak)) int _unlink (const char *path) {
	errno = EPERM;
	return -1;
}
