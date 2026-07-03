// SPDX-License-Identifier: GPL-2.0-only
// 20260504 (c) William Fonkou Tambe

#ifndef __LIBGLOSS_RISCV__OS_H
#define __LIBGLOSS_RISCV__OS_H

#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Cast a member of a structure out to the containing structure.
// PTR:    The pointer to the member.
// TYPE:   The type of the container struct this is embedded in.
// MEMBER: The name of the member within the struct.
#define container_of(PTR, TYPE, MEMBER) ({ \
	typeof(((TYPE *)0)->MEMBER) *__ptr = (PTR); \
	(TYPE *)((char *)__ptr - offsetof(TYPE, MEMBER)); })

#define _atomic_op(OP, PTR, VAL) ({ \
	__typeof__(PTR) __ptr = (PTR); \
	__typeof__(VAL) __val = (VAL); \
	__typeof__(*(PTR)) __ret; \
	switch (__SIZEOF_POINTER__) { \
		case 4: \
			__asm__ __volatile__ ( \
				#OP ".w.aqrl %0, %2, %1\n" \
				: "=r" (__ret), "+A" (*__ptr) \
				: "r" (__val) \
				: "memory"); \
			break; \
		case 8: \
			__asm__ __volatile__ ( \
				#OP ".d.aqrl %0, %2, %1\n" \
				: "=r" (__ret), "+A" (*__ptr) \
				: "r" (__val) \
				: "memory"); \
			break; \
	} __ret; })
#define _xchg(PTR, VAL)       _atomic_op(amoswap, PTR, VAL)
#define _atomic_add(PTR, VAL) _atomic_op(amoadd, PTR, VAL)
#define _atomic_inc(PTR)      _atomic_add(PTR, 1)
#define _atomic_dec(PTR)      _atomic_add(PTR, -1)
#define _atomic_and(PTR, VAL) _atomic_op(amoand, PTR, VAL)
#define _atomic_or(PTR, VAL)  _atomic_op(amoor, PTR, VAL)
#define _atomic_xor(PTR, VAL) _atomic_op(amoxor, PTR, VAL)

typedef struct _dlist {
	struct _dlist *prev;
	struct _dlist *next;
} _dlist_t;

#define _DLIST_NIL (_dlist_t){0, 0}

#define _dlist_init(X) ({ \
	(X)->next = (X); \
	(X)->prev = (X); })

// Insert an entry between two known consecutive entries.
static inline void _dlist_add (_dlist_t *l, _dlist_t *prev, _dlist_t *next) {
	next->prev = l;
	l->next = next;
	l->prev = prev;
	prev->next = l;
}

// Remove one or more entries between two known entries.
static inline void _dlist_del (_dlist_t *prev, _dlist_t *next) {
	next->prev = prev;
	prev->next = next;
}

typedef uint64_t _date_t;
#define _DATE_MAX (-(_date_t)1)

typedef struct _timer {
	_dlist_t l;
	_date_t e; // Expiration date.
	uintptr_t cpu; // A _timer can only be re-armed or dis-armed by the CPU that armed it.
	void (*f)(struct _timer *);
} _timer_t;

#define _TIMER_NIL (_timer_t){_DLIST_NIL, 0, 0, (void *)0}

#define _TIMER_DEF(X, F) _timer_t X = {_DLIST_NIL, 0, 0, (F)}

#define _timer_init(X, F) ({ \
	(X)->l = _DLIST_NIL; \
	(X)->f = F; })

void _timer_arm (_timer_t *t, _date_t e);
void _timer_disarm (_timer_t *t);

typedef struct _irq {
	_dlist_t l;
	uintptr_t n; // Interrupt number.
	void (*f)(struct _irq *);
} _irq_t;

#define _IRQ_NIL (_irq_t){_DLIST_NIL, 0, 0}

#define _IRQ_DEF(X, N, F) _irq_t X = {_DLIST_NIL, (N), (F)}

#define _irq_init(X, N, F) ({ \
	(X)->l = _DLIST_NIL; \
	(X)->n = N; \
	(X)->f = F; })

void _irq_register (_irq_t *i);
void _irq_unregister (_irq_t *i);

typedef struct {
	uintptr_t lock;
	uintptr_t p; // Count of threads about to add themselves to the wait-queue;
	             // used to avoid mutex and fifo race conditions missing wake-up calls;
	             // incremented, within the wait-queue lock, before the thread
	             // sleeps, and decremented by _thread_sleeponwquntil() once the
	             // thread is on the wait-queue; only ever accessed using plain
	             // loads and stores (no atomics).
	void *l; // Points to a circular linked list of _thread_t(s) waiting.
} _waitq_t;

#define _WAITQ_NIL (_waitq_t){0, 0, 0}

typedef struct {
	uintptr_t lock;
	void* owner; // Points to _thread_t which acquired the mutex.
	uintptr_t acqcnt; // Acquisition count.
	_waitq_t waitq; // Used by _thread_t(s) waiting on this _mutex_t.
} _mutex_t;

#define _MUTEX_NIL (_mutex_t){0, 0, 0, _WAITQ_NIL}

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

#define _fifo_init(X, B, S) ({ \
	_xchg(&(X)->lock, 0); \
	(X)->widx = 0; (X)->ridx = 0; \
	(X)->buf = B; (X)->sz = S; \
	(X)->wwaitq = _WAITQ_NIL; \
	(X)->rwaitq = _WAITQ_NIL; })

size_t _fifo_put (_fifo_t *f, void *buf, size_t sz, _date_t timeout);
size_t _fifo_get (_fifo_t *f, void *buf, size_t sz, bool peek, _date_t timeout);
#define _fifo_flush(X) _fifo_get((X), 0, -1, false, 0) /* Empty the buffer */
size_t _fifo_usage (_fifo_t *f);
void _fifo_rst (_fifo_t *f);

#define _sem_t _fifo_t
#define _SEM_DEF(X, N, I) _sem_t X = {0, (I), 0, 0, (N), _WAITQ_NIL, _WAITQ_NIL}; static_assert(((I) <= (N)), "I > N")
#define _sem_init(X, N, I) ({ if ((I) > (N)) _oops(); _fifo_init((X), 0, (N)); (void)((X)->widx = (I)); })
#define _sem_put(X, T) ({ size_t ret = 1; if (!_fifo_put((X), 0, 1, (T))) ret = 0; ret; })
#define _sem_get(X, T) ({ size_t ret = 1; if (!_fifo_get((X), 0, 1, false, (T))) ret = 0; ret; })
#define _sem_rst(X) _fifo_rst(X)

typedef struct {
	uintptr_t ra, sp;
	uintptr_t t0, t1, t2, s0, s1;
	uintptr_t a0, a1, a2, a3, a4, a5, a6, a7;
	uintptr_t s2, s3, s4, s5, s6, s7, s8, s9;
	uintptr_t s10, s11, t3, t4, t5, t6;
	// Above valid only when interrupting a thread or the handling of a trap.
	uintptr_t scratch, status, epc;
	uintptr_t tval, tval2, cause;
} _savedctx_t;

typedef struct {
	_dlist_t l; // Circular linked list of either running or stopped (on _waitq_t) threads.
	enum {
		_THREAD_STOPPED = 0,
		_THREAD_RUNNING = 1
	} state;
	_waitq_t *wq; // Non-null when state is _THREAD_STOPPED.
	              // Can also be null while state is _THREAD_STOPPED.
	uintptr_t claim; // Serializes waking the thread between __thread_wakeup()
	                 // and _thread_schedoncpu() running on different CPUs.
	uintptr_t ctxsaved; // Set by __switchctx() once the context of the thread
	                    // being switched-out is fully saved; the CPU resuming
	                    // the thread waits on it, then clears it, so that a
	                    // woken-up thread cannot be restored from a stale or
	                    // partially saved context while its previous CPU is
	                    // still switching it out.
	_timer_t z; // Used to make the thread sleep for a duration.
	_date_t timeleft; // Time left to run when non-null.
	void *stack; // Start of the stack.
	uintptr_t cpu; // Used by _thread_sched() to index the __runq to use.
	bool pin; // When true, the thread does not migrate.
	uintptr_t irq_disabled; // For this thread, when null IRQs are enabled, otherwise they are disabled.
	// Pointer to context-switch save area; where only following fields are used:
	// ra, s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, scratch, status.
	_savedctx_t *savedctx;
} _thread_t; // Its size must be a multiple of sizeof(uintptr_t).

register _thread_t *_thread_cur __asm__ ("tp");

// Return non-null only when handling a trap.
#define _trap_savedctx() ({ \
	_savedctx_t *x; \
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

_date_t _clkcycles (void);

#define _SECS(D) ({ \
	_date_t d = _clkfreq(); \
	d = ((D)*d); /* Multiply by the 64bits local so it cannot overflow */ \
	d; })
#define _MSECS(D) ({ \
	_date_t d = _clkfreq(); \
	d = ((d >= 1000) ? ((D)*(d/1000)) : (((D)*d)/1000)); \
	d; })
#define _USECS(D) ({ \
	_date_t d = _clkfreq(); \
	d = ((d >= 1000000) ? ((D)*(d/1000000)) : (((D)*d)/1000000)); \
	d; })
#define _NSECS(D) ({ \
	_date_t d = _clkfreq(); \
	d = ((d >= 1000000000) ? ((D)*(d/1000000000)) : (((D)*d)/1000000000)); \
	d; })

// Print diagnosis info and shutdown.
#define _oops() ({ \
	/* Trap using a syscall which captures the pc. */ \
	register uintptr_t syscall_id asm("a7") = -1; \
	__asm__ __volatile__ ("ecall" :: "r"(syscall_id)); })

void _preempt_disable (void);
void _preempt_enable (void);

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
#define _thread_sleeponwq(Q,D) ({ \
	if ((D) == _DATE_MAX) \
		_thread_sleeponwquntil((Q), _DATE_MAX); \
	else { /* For accuracy, there must not be preemption between the sleep
		      duration computation and the call to _thread_sleeponwquntil(). */ \
		_preempt_disable(); \
		_thread_sleeponwquntil((Q), (_clkcycles() + (D))); \
		_preempt_enable(); \
	}; })
#define _thread_sleepuntil(D) _thread_sleeponwquntil(0, (D))
#define _thread_sleep(D) _thread_sleeponwq(0, (D))
void _thread_exit (void);

#define _is_thread_stopped(X) ((X)->state == _THREAD_STOPPED)
#define _is_thread_terminated(X) (/*_is_thread_stopped(X) &&*/ !(X)->savedctx)
#define _is_thread_running(X) ((X)->state == _THREAD_RUNNING)

uintptr_t _ncpu (void);

#endif /* __LIBGLOSS_RISCV__OS_H */
