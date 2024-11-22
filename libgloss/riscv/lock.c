// SPDX-License-Identifier: GPL-2.0-only
// 20241218 (c) William Fonkou Tambe

#include <stdint.h>
#include <stdlib.h>

#include <sys/lock.h>

#include "_os.h"

struct __lock {
	_mutex_t m;
};

struct __lock __lock___sfp_recursive_mutex = _MUTEX_CLR;
struct __lock __lock___atexit_recursive_mutex = _MUTEX_CLR;
struct __lock __lock___at_quick_exit_mutex = _MUTEX_CLR;
struct __lock __lock___malloc_recursive_mutex = _MUTEX_CLR;
struct __lock __lock___env_recursive_mutex = _MUTEX_CLR;
struct __lock __lock___tz_mutex = _MUTEX_CLR;
struct __lock __lock___dd_hash_mutex = _MUTEX_CLR;
struct __lock __lock___arc4random_mutex = _MUTEX_CLR;

void __retarget_lock_init (_LOCK_T *lock) {
	struct __lock *l = malloc(sizeof(struct __lock));
	l->m = (_mutex_t)_MUTEX_CLR;
	*lock = l;
}

void __retarget_lock_init_recursive (_LOCK_T *lock) {
	__retarget_lock_init(lock);
}

void __retarget_lock_close (_LOCK_T lock) {
	free(lock);
}
void __retarget_lock_close_recursive (_LOCK_T lock) {
	free(lock);
}

void __retarget_lock_acquire (_LOCK_T lock) {
	while (!_mutex_lock(&lock->m, -1));
}

void __retarget_lock_acquire_recursive (_LOCK_T lock) {
	while (!_mutex_lock_recursive(&lock->m, -1));
}

int __retarget_lock_try_acquire (_LOCK_T lock) {
	return _mutex_lock(&lock->m, 0);
}

int __retarget_lock_try_acquire_recursive (_LOCK_T lock) {
	return _mutex_lock_recursive(&lock->m, 0);
}

void __retarget_lock_release (_LOCK_T lock) {
	_mutex_unlock(&lock->m);
}

void __retarget_lock_release_recursive (_LOCK_T lock) {
	_mutex_unlock_recursive(&lock->m);
}
