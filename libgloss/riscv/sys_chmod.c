// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/types.h>

__attribute__((weak)) int _chmod (const char *path, mode_t mode) {
	return -1;
}
