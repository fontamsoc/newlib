// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

// Close a file.
__attribute__((weak)) int _close (int fd) {
	errno = EPERM;
	return -1;
}
