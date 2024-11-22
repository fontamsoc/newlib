// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <errno.h>

#include "kernel_stat.h"

// Status of a file (by name).
__attribute__((weak)) int _stat (const char *file, struct stat *st) {
	errno = EPERM;
	return -1;
}
