// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/types.h>

__attribute__((weak)) char* _getcwd (char *buf, size_t size) {
	return NULL;
}
