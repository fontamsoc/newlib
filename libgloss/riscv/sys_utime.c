// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <machine/syscall.h>
#include <sys/types.h>
#include <utime.h>
#include <errno.h>

__attribute__((weak)) int _utime (const char *path, const struct utimbuf *times) {
	errno = EPERM;
	return -1;
}
