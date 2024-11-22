// SPDX-License-Identifier: GPL-2.0-only
// 20241218 (c) William Fonkou Tambe

#include <stdint.h>

#include <malloc.h>
#include <sys/lock.h>

#include "_os.h"

__LOCK_INIT_RECURSIVE(static, __malloc_recursive_mutex);

void __malloc_lock (struct _reent *ptr) {
	_preempt_disable();
	__lock_acquire_recursive (__malloc_recursive_mutex);
}

void __malloc_unlock (struct _reent *ptr) {
	__lock_release_recursive (__malloc_recursive_mutex);
	_preempt_enable();
}
