// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/types.h>

__attribute__((weak)) int _chown (const char *path, uid_t owner, gid_t group) {
	return -1;
}
