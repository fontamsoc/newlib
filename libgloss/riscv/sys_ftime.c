// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/timeb.h>

// Get the current time. Only relatively correct.
__attribute__((weak)) int _ftime (struct timeb *tp) {
	tp->time = tp->millitm = 0;
	return 0;
}
