// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/types.h>

#include "_os_params.h"

__attribute__((weak)) int _stdin_echo = 0; // Set non-null for echoing.
static char __getchar (void) {
	char c = *(volatile char *)SERIAL0_ADDR;
	if (_stdin_echo) {
		*(volatile char *)SERIAL0_ADDR = c;
		if (c == '\r')
			*(volatile char *)SERIAL0_ADDR = '\n';
	}
	return c;
}

static int __read_stdin (void *ptr, int len) {
	*(char *)ptr = __getchar();
	return 1;
}

typedef int (*_read_tbl_fn_t) (void *ptr, int len);
__attribute__((weak)) _read_tbl_fn_t *_read_tbl =
	(_read_tbl_fn_t[]){__read_stdin};
__attribute__((weak)) int _read_tblcnt = 1;

// Read from a file.
__attribute__((weak)) ssize_t _read (int fd, void *ptr, size_t len) {
	if (fd >= _read_tblcnt)
		return -1;
	if (len > 0)
		return _read_tbl[fd](ptr, len);
	return 0;
}
