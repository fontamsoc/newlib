// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

__attribute__((weak)) int _chdir (const char *path) {
	return -1;
}
