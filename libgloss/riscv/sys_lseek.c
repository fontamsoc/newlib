// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/types.h>
#include <errno.h>

// Set position in a file.
__attribute__((weak)) off_t _lseek (int file, off_t ptr, int dir) {
	errno = EPERM;
	return -1;
}
