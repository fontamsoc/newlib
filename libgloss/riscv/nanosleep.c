// SPDX-License-Identifier: GPL-2.0-only
// 20260504 (c) William Fonkou Tambe

#include <errno.h>
#include <machine/syscall.h>
#include <sys/time.h>

#include "_os.h"

int nanosleep (const struct timespec *rqtp, struct timespec *rmtp) {
	if (!rqtp || rqtp->tv_nsec < 0 || rqtp->tv_nsec > 999999999) {
		errno = EINVAL;
		return -1;
	}
	// Computed using 64bits _date_t so it cannot overflow.
	_thread_sleep(_SECS((_date_t)rqtp->tv_sec) + _NSECS((_date_t)rqtp->tv_nsec));
	if (rmtp) {
		rmtp->tv_sec = 0;
		rmtp->tv_nsec = 0;
	}
	return 0;
}
