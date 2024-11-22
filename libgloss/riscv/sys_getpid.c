// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

/* Get process id. This is sometimes used to generate strings unlikely
   to conflict with other processes. Minimal implementation for a
   system without processes just returns 1.  */
__attribute__((weak)) int _getpid() {
	return 1;
}
