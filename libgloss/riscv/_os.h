// SPDX-License-Identifier: GPL-2.0-only
// 20250924 (c) William Fonkou Tambe

#ifndef __LIBGLOSS_RISCV__OS_H
#define __LIBGLOSS_RISCV__OS_H

#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

// Cast a member of a structure out to the containing structure.
// ptr:    The pointer to the member.
// type:   The type of the container struct this is embedded in.
// member: The name of the member within the struct.
#define container_of(ptr, type, member) ({ \
	typeof(((type *)0)->member) *__mptr = (ptr); \
	(type *)((char *)__mptr - offsetof(type, member)); })

#define _atomic_op(op, ptr, val) ({ \
	__typeof__(ptr) __ptr = (ptr); \
	__typeof__(val) __val = (val); \
	__typeof__(*(ptr)) __ret; \
	switch (__SIZEOF_POINTER__) { \
		case 4: \
			__asm__ __volatile__ ( \
				#op ".w.aqrl %0, %2, %1\n" \
				: "=r" (__ret), "+A" (*__ptr) \
				: "r" (__val) \
				: "memory"); \
			break; \
		case 8: \
			__asm__ __volatile__ ( \
				#op ".d.aqrl %0, %2, %1\n" \
				: "=r" (__ret), "+A" (*__ptr) \
				: "r" (__val) \
				: "memory"); \
			break; \
	} __ret; })
#define _xchg(ptr, val)       _atomic_op(amoswap, ptr, val)
#define _atomic_add(ptr, val) _atomic_op(amoadd, ptr, val)
#define _atomic_inc(ptr)      _atomic_add(ptr, 1)
#define _atomic_dec(ptr)      _atomic_add(ptr, -1)
#define _atomic_and(ptr, val) _atomic_op(amoand, ptr, val)
#define _atomic_or(ptr, val)  _atomic_op(amoor, ptr, val)
#define _atomic_xor(ptr, val) _atomic_op(amoxor, ptr, val)

struct _dlist {
	struct _dlist *prev;
	struct _dlist *next;
};

#define _DLIST_CLR {0, 0}

static inline void _dlist_clr (struct _dlist *l) {
	l->next = 0;
	l->prev = 0;
}

static inline void _dlist_init (struct _dlist *l) {
	l->next = l;
	l->prev = l;
}

// Insert an entry between two known consecutive entries.
static inline void _dlist_add (struct _dlist *l, struct _dlist *prev, struct _dlist *next) {
	next->prev = l;
	l->next = next;
	l->prev = prev;
	prev->next = l;
}

// Remove one or more entries between two known entries.
static inline void _dlist_del (struct _dlist *prev, struct _dlist *next) {
	next->prev = prev;
	prev->next = next;
}

typedef uint64_t _date_t;
#define _DATE_MAX (-(_date_t)1)

struct _timer {
	struct _dlist l;
	_date_t e; // Expiration date.
	void (*f)(struct _timer *);
};

typedef struct _timer _timer_t;

#define _TIMER_CLR {_DLIST_CLR, 0, 0}

#define _timer_init(T, F) ({ \
	(T)->l.prev = 0; \
	(T)->l.next = 0; \
	(T)->f = F; })
void _timer_arm (_timer_t *t, _date_t e);
void _timer_disarm (_timer_t *t);

struct _irq {
	struct _dlist l;
	uintptr_t n; // Interrupt number.
	void (*f)(struct _irq *);
};

typedef struct _irq _irq_t;

#define _IRQ_CLR {_DLIST_CLR, 0, 0}

#define _irq_init(I, N, F) ({ \
	(I)->l.prev = 0; \
	(I)->l.next = 0; \
	(I)->n = N; \
	(I)->f = F; })
void _irq_register (_irq_t *i);
void _irq_unregister (_irq_t *i);

typedef struct {
	uintptr_t lock;
	uintptr_t p; // Used to avoid mutex and fifo race conditions.
	void *l; // Points to a circular linked list of _thread_t(s) waiting.
} _waitq_t;

#define _WAITQ_CLR {0, 0, 0}

typedef struct {
	uintptr_t lock;
	void* owner; // _tpval() when mutex was acquired.
	uintptr_t acqcnt; // Acquisition count.
	_waitq_t waitq; // Used by _thread_t(s) waiting on this _mutex_t.
} _mutex_t;

#define _MUTEX_CLR {0, 0, 0, _WAITQ_CLR}

uintptr_t _mutex_lock (_mutex_t *m, _date_t timeout);
void _mutex_unlock (_mutex_t *m);
uintptr_t _mutex_lock_recursive (_mutex_t *m, _date_t timeout);
void _mutex_unlock_recursive (_mutex_t *m);

typedef struct {
	uintptr_t lock;
	uintptr_t widx;
	uintptr_t ridx;
	void *buf;
	size_t sz;
	_waitq_t wwaitq; // Used by _thread_t(s) waiting from _fifo_put().
	_waitq_t rwaitq; // Used by _thread_t(s) waiting from _fifo_get().
} _fifo_t;

void _fifo_init (_fifo_t *f, void *buf, size_t sz);
size_t _fifo_put (_fifo_t *f, void *buf, size_t sz, _date_t timeout);
size_t _fifo_get (_fifo_t *f, void *buf, size_t sz, bool peek, _date_t timeout);
#define _fifo_flush(X) _fifo_get((X), 0, -1, false, 0) /* Empty the buffer */
size_t _fifo_usage (_fifo_t *f);
void _fifo_rst (_fifo_t *f);

#define _sem_t _fifo_t
#define _SEM_DEF(X, N, I) _sem_t X = {0, I, 0, 0, N, _WAITQ_CLR, _WAITQ_CLR}; static_assert(((I) <= (N)), "I > N")
#define _sem_init(X, N, I) ({ if ((I) > (N)) _oops(); _fifo_init((X), 0, (N)); (void)((X)->widx = (I)); })
#define _sem_put(X, T) ({ size_t ret = 1; if (!_fifo_put((X), 0, 1, (T))) ret = 0; ret; })
#define _sem_get(X, T) ({ size_t ret = 1; if (!_fifo_get((X), 0, 1, false, (T))) ret = 0; ret; })
#define _sem_rst(X) _fifo_rst(X)

typedef struct {
	struct _dlist l; // Circular linked list of either running or stopped (on _waitq_t) threads.
	enum {
		_THREAD_STOPPED = 0,
		_THREAD_RUNNING = 1
	} state;
	_waitq_t *wq; // Non-null when state is _THREAD_STOPPED.
	              // Can also be null while state is _THREAD_STOPPED.
	_timer_t z; // Used to make the thread sleep for a duration.
	_date_t timeleft; // Time left to run when non-null.
	void *stack; // Start of the stack.
	uintptr_t cpu; // Used by _thread_sched() to index the __runq to use.
	bool pin; // When true, the thread does not migrate.
	uintptr_t irq_disabled; // For this thread, when null IRQs are enabled, otherwise they are disabled.
	struct {
		uintptr_t ra, sp;
		uintptr_t s0, s1, s2, s3, s4, s5;
		uintptr_t s6, s7, s8, s9, s10, s11;
		uintptr_t scratch; // When non-null, the saved context it points-to must be used instead.
		uintptr_t status;
	} savedctx; // Save area for context switching.
} _thread_t; // Its size must be a multiple of sizeof(uintptr_t).

register _thread_t *_thread_cur __asm__ ("tp");

typedef struct {
	uintptr_t ra, sp;
	uintptr_t t0, t1, t2, s0, s1;
	uintptr_t a0, a1, a2, a3, a4, a5, a6, a7;
	uintptr_t s2, s3, s4, s5, s6, s7, s8, s9;
	uintptr_t s10, s11, t3, t4, t5, t6;
	// Above valid only when interrupting a thread or the handling of a trap.
	uintptr_t scratch, status, epc;
	uintptr_t tval, tval2, cause;
} _trap_savedctx_t;

// Return non-null only when handling a trap.
#define _trap_savedctx() ({ \
	_trap_savedctx_t *x; \
	__asm__ __volatile__ ("csrr %0, mscratch\n" : "=r"(x) :: "memory"); \
	x; })

#define _cpuid() ({ \
	uintptr_t x; \
	__asm__ __volatile__ ("csrr %0, mhartid\n" : "=r"(x) :: "memory"); \
	x; })

#define _clkfreq() ({ \
	uintptr_t x; \
	__asm__ __volatile__ ("csrr %0, 0xcc0\n" : "=r"(x) :: "memory"); \
	x; })

// Print diagnosis info and shutdown.
#define _oops() ({ \
	/* Trap using a syscall which captures the pc. */ \
	register uintptr_t syscall_id asm("a7") = -1; \
	__asm__ __volatile__ ("ecall" :: "r"(syscall_id)); })

void _preempt_disable (void);
void _preempt_enable (void);

_date_t _clkcycles (void);

#define _SECS(X) ({ \
	_date_t x = _clkfreq(); \
	x = ((X)*_clkfreq()); \
	x; })
#define _MSECS(X) ({ \
	_date_t x = _clkfreq(); \
	x = ((x >= 1000) ? ((X)*(x/1000)) : (((X)*x)/1000)); \
	x; })
#define _USECS(X) ({ \
	_date_t x = _clkfreq(); \
	x = ((x >= 1000000) ? ((X)*(x/1000000)) : (((X)*x)/1000000)); \
	x; })
#define _NSECS(X) ({ \
	_date_t x = _clkfreq(); \
	x = ((x >= 1000000000) ? ((X)*(x/1000000000)) : (((X)*x)/1000000000)); \
	x; })

_thread_t *_thread_create (void* stack, uintptr_t stacksz, void (*entry)(void *arg), void *arg);
void _thread_sched (_thread_t *thrd);
void _thread_schedoncpu (_thread_t *thrd, uintptr_t cpu, bool pin);
void _thread_stop (_thread_t *thrd);
void _thread_kill (_thread_t *thrd);
void _thread_dispose (_thread_t *thrd);
void _thread_schedone (_waitq_t *wq);
void _thread_schedall (_waitq_t *wq);
void _thread_preempt (uintptr_t cpu);
#define _thread_yield() _thread_preempt(_cpuid())
void _thread_sleeponwquntil (_waitq_t *wq, _date_t e);
#define _thread_sleeponwq(X,T) ({ \
	if ((T) == _DATE_MAX) \
		_thread_sleeponwquntil((X), _DATE_MAX); \
	else { /* For accuracy, there must not be preemption between the sleep
		      duration computation and the call to _thread_sleeponwquntil(). */ \
		_preempt_disable(); \
		_thread_sleeponwquntil((X), (_clkcycles() + (T))); \
		_preempt_enable(); \
	}; })
#define _thread_sleepuntil(T) _thread_sleeponwquntil(0, (T))
#define _thread_sleep(T) ({ \
	if ((T) == _DATE_MAX) \
		_thread_sleepuntil(_DATE_MAX); \
	else { /* For accuracy, there must not be preemption between the sleep
		      duration computation and the call to _thread_sleepuntil(). */ \
		_preempt_disable(); \
		_thread_sleepuntil(_clkcycles() + (T)); \
		_preempt_enable(); \
	}; })
void _thread_exit (void);

#define _is_thread_stopped(X) ((X)->state == _THREAD_STOPPED)
#define _is_thread_terminated(X) (/*_is_thread_stopped(X) &&*/ !(X)->savedctx.sp)
#define _is_thread_running(X) ((X)->state == _THREAD_RUNNING)

uintptr_t _ncpu (void);

#endif /* __LIBGLOSS_RISCV__OS_H */
