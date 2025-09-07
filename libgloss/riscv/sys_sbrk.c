// SPDX-License-Identifier: GPL-2.0-only
// 20250907 (c) William Fonkou Tambe

#include <sys/types.h>
#include <errno.h>

/* Increase program data space. As malloc and related functions depend
   on this, it is useful to have a working implementation. The following
   is suggested by the newlib docs and suffices for a standalone
   system.  */
__attribute__((weak)) void* _sbrk (ptrdiff_t incr) {
	extern char _end[];
	extern uintptr_t __heap_ptr;
	extern uintptr_t __heap_end;
	uintptr_t brk = (__heap_ptr + incr);
	if (brk >= (uintptr_t)_end && brk < __heap_end) {
		void *ret = (void *)__heap_ptr;
		__heap_ptr = brk;
		return ret;
	}
	errno = ENOMEM;
	return (void *)-1;
}
