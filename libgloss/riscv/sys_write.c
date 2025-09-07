// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/types.h>

#include "_os_params.h"

static void __printstrn (char *s, int sz) {
	for (; sz > 0; --sz, ++s)
		*(volatile char *)SERIAL0_ADDR = *s;
}

static int __write_stdout (void *ptr, int len) {
	__printstrn((char *)ptr, len);
	return len;
}

typedef int (*_write_tbl_fn_t) (void *ptr, int len);
__attribute__((weak)) _write_tbl_fn_t *_write_tbl =
	(_write_tbl_fn_t[]){0, __write_stdout, __write_stdout};
__attribute__((weak)) int _write_tblcnt = 3;

// Write to a file.
__attribute__((weak)) ssize_t _write (int fd, void *ptr, size_t len) {
	if (fd >= _write_tblcnt)
		return -1;
	_write_tbl_fn_t fn = _write_tbl[fd];
	if (fn && len > 0)
		return fn(ptr, len);
	return 0;
}
