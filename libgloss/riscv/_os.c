// SPDX-License-Identifier: GPL-2.0-only
// 20241203 (c) William Fonkou Tambe

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "_os.h"
#include "_os_params.h"

#include <machine/syscall.h>
#include <machine/hwdrvirqctrl.h>

#define __hexdigit(D) ({ char d = (D); (d+((d>=10)?('a'-10):'0')); })
#define __printhex(I) ({ \
	typeof(I) Ival = (I); \
	unsigned Isz = sizeof(I); \
	for (unsigned i = 0; i < (2*Isz); ++i) \
		*(volatile char *)SERIAL0_ADDR = \
			__hexdigit((Ival>>(((8*Isz)-4)-(i*4)))&0xf); \
})
static void __printdec (uintptr_t i) {
    if (i / 10)
        __printdec(i / 10);
    *(volatile char *)SERIAL0_ADDR = ((i % 10) + '0');
}
static void __printstr (char *s) {
	for (char c; c = *s; ++s)
		*(volatile char *)SERIAL0_ADDR = c;
}
static void __printstrn (char *s, size_t sz) {
	for (; sz > 0; --sz, ++s)
		*(volatile char *)SERIAL0_ADDR = *s;
}

uint8_t __trap_stacks[NCPU][TRAP_STACK_SIZE];

bool __trap_exc_ecall_m (void) {
	_trap_savedctx_t *savedctx = _trap_savedctx();
	switch (savedctx->a7) {
		case SYS_brk:
			extern char _end[];
			extern uintptr_t __heap_ptr;
			extern uintptr_t __heap_end;
			if (savedctx->a0 >= (uintptr_t)_end && savedctx->a0 < __heap_end)
				__heap_ptr = savedctx->a0;
			savedctx->a0 = __heap_ptr;
			break;
		case SYS_read:
			if (savedctx->a0 == 0) {
				char *ptr = (char *)savedctx->a1;
				if (savedctx->a2 > 0) { //for (int i = savedctx->a2; i > 0; --i, ++ptr)
					*ptr = *(volatile char *)SERIAL0_ADDR;
					if (0) { // Do this for echoing ...
						*(volatile char *)SERIAL0_ADDR = *ptr;
						if (*ptr == '\r')
							*(volatile char *)SERIAL0_ADDR = '\n';
					}
				}
				savedctx->a0 = 1; //savedctx->a2;
			} else
				savedctx->a0 = -1;
			break;
		case SYS_write:
			if (savedctx->a0 == 1 || savedctx->a0 == 2) {
				__printstrn((char *)savedctx->a1, (size_t)savedctx->a2);
				savedctx->a0 = savedctx->a2;
			} else
				savedctx->a0 = -1;
			break;
		case -1:
			__printstr("==== OOPS CPU"); __printdec(_cpuid());
			__printstr(" 0x"); __printhex(savedctx->epc);
			__printstr(" ====\n");
		case SYS_exit:
			__asm__ __volatile__ ("csrw mtvec, x0; ebreak; 0:; j 0b\n" ::: "memory");
		default:
			savedctx->a0 = -1;
	}
	return true;
}

// Thread-local IRQ status; when null IRQs are enabled, otherwise they are disabled.
static __thread uintptr_t __irq_disabled = 0;

void _preempt_disable (void) {
	if (_trap_savedctx()) // Do nothing if handling a trap.
		return;
	if (__irq_disabled++)
		return;
	__asm__ __volatile__ ("csrc mstatus, 0x8\n" ::: "memory"); // Clear mstatus.mie.
}

void _preempt_enable (void) {
	if (_trap_savedctx()) // Do nothing if handling a trap.
		return;
	if (!__irq_disabled) {
		_oops();
		return;
	}
	if (--__irq_disabled)
		return;
	__asm__ __volatile__ ("csrs mstatus, 0x8\n" ::: "memory"); // Set mstatus.mie.
}

_date_t _clkcycles (void) {
#if __riscv_xlen == 64
	uint64_t lo;
	__asm__ __volatile__ ("csrr %0, cycle\n" : "=r"(lo) :: "memory");
	return lo;
#elif __riscv_xlen == 32
	inline uint32_t __clkcycles_hi (void) {
		uint32_t hi;
		__asm__ __volatile__ ("csrr %0, cycleh\n" : "=r"(hi) :: "memory");
		return hi;
	}
	uint32_t lo, hi;
	do {
		hi = __clkcycles_hi();
		__asm__ __volatile__ ("csrr %0, cycle\n" : "=r"(lo) :: "memory");
	} while (hi != __clkcycles_hi());
	return (((uint64_t)hi << 32) | lo);
#else
#error "Unexpected __riscv_xlen"
#endif
}

static void __settimecmp (_date_t e) {
#if __riscv_xlen == 64
	__asm__ __volatile__ (
		"csrw 0x34d, %0\n" /* mtimecmp */
		:: "r"(e)
		:  "memory");
#elif __riscv_xlen == 32
	__asm__ __volatile__ (
		// Set using ordering described in the priviledge isa manual.
		//"csrw 0x34d, %2\n" /* mtimecmp */ /* No smaller than old value */
		"csrw 0x35d, %1\n" /* mtimecmph */ /* No smaller than new value */
		"csrw 0x34d, %0\n" /* mtimecmp */ /* New value */
		:: "r"((uint32_t)e), "r"((uint32_t)(e >> 32))/*, "r"((uint32_t)-1)*/
		:  "memory");
#else
#error "Unexpected __riscv_xlen"
#endif
}

// Per CPU list of timers ordered from nearest to farthest.
// Each element of the array is to be used and modified only by the corresponding CPU.
// It must be modified with IRQs disabled.
static _timer_t *__timer_list[NCPU] = {[0 ... NCPU-1] = 0};

// Orderly insert or re-insert a _timer in __timer_list[_cpuid()].
void _timer_arm (_timer_t *t, _date_t e) {
	_preempt_disable();
	uintptr_t coreid = _cpuid();
	if (t->l.prev) {
		if (!t->l.next)
			_oops();
		if (t->l.next != &t->l) {
			if (t == __timer_list[coreid])
				__timer_list[coreid] = container_of(t->l.next, _timer_t, l);
			_dlist_del(t->l.prev, t->l.next);
		} else
			__timer_list[coreid] = 0;
	}
	t->e = e;
	if (__timer_list[coreid]) {
		uintptr_t uh = 1; // Determine whether to update __timer_list[coreid].
		_timer_t *th = __timer_list[coreid];
		while (e >= th->e) {
			th = container_of(th->l.next, _timer_t, l);
			if (th == __timer_list[coreid]) {
				uh = 0;
				break;
			}
		}
		_dlist_add(&t->l, th->l.prev, &th->l);
		if (uh && th == __timer_list[coreid]) {
			__timer_list[coreid] = t;
			__settimecmp(__timer_list[coreid]->e);
		}
	} else {
		_dlist_init(&t->l);
		__timer_list[coreid] = t;
		__settimecmp(__timer_list[coreid]->e);
	}
	__asm__ __volatile__ ("csrs mie, %0\n" :: "r"(0x80) : "memory"); // Set mie.mtie.
	_preempt_enable();
}

// Remove a _timer from __timer_list[_cpuid()].
void _timer_disarm (_timer_t *t) {
	_preempt_disable();
	uintptr_t coreid = _cpuid();
	if (t->l.next != &t->l) {
		if (t == __timer_list[coreid]) {
			__timer_list[coreid] = container_of(t->l.next, _timer_t, l);
			__settimecmp(__timer_list[coreid]->e);
		}
		_dlist_del(t->l.prev, t->l.next);
	} else
		__timer_list[coreid] = 0;
	_dlist_clr(&t->l);
	if (!__timer_list[coreid])
		__asm__ __volatile__ ("csrc mie, %0\n" :: "r"(0x80) : "memory"); // Clear mie.mtie.
	_preempt_enable();
}

// List of registered IRQs.
// A single circular linked-list shared by all CPUs is used,
// so that it is not necessary to register an _irq for each CPU.
static struct {
	uintptr_t lock;
	_irq_t *l;
} __irqs = {0, 0};

// Register an IRQ by adding its _irq to
// the circular linked-list pointed by __irqs.l.
void _irq_register (_irq_t *i) {
	_preempt_disable();
	while (_xchg(&__irqs.lock, 1));
	if (i->l.prev || i->l.next)
		_oops();
	if (__irqs.l) {
		_irq_t *h = __irqs.l;
		_dlist_add(&i->l, h->l.prev, &h->l);
	} else {
		_dlist_init(&i->l);
		__irqs.l = i;
	}
	_xchg(&__irqs.lock, 0);
	_preempt_enable();
}

// Unregister an IRQ by removing its _irq from
// the circular linked-list pointed by __irqs.l.
void _irq_unregister (_irq_t *i) {
	_preempt_disable();
	while (_xchg(&__irqs.lock, 1));
	if (i->l.next != &i->l) {
		if (i == __irqs.l)
			__irqs.l = container_of(i->l.next, _irq_t, l);
		_dlist_del(i->l.prev, i->l.next);
	} else
		__irqs.l = 0;
	_dlist_clr(&i->l);
	_xchg(&__irqs.lock, 0);
	_preempt_enable();
}

uintptr_t irqctrl_lock = 0; // Lock used to serialize access to the irqctrl device.

// Triggers an interrupt on the CPU given as argument.
// Returns the CPU number if valid, otherwise returns -1.
static uintptr_t __irq_ipi (uintptr_t cpu) {
	uintptr_t ret;
	do {
		while (_xchg(&irqctrl_lock, 1));
		ret = hwdrvirqctrl_int(cpu);
		_xchg(&irqctrl_lock, 0);
	} while (ret == -2);
	return ret;
}

// Acknowledge interrupt and return its number.
// The argument `en` enable/disable further interrupt delivery to _cpuid().
// Returns -2 if there are no pending interrupt, or -1 for an IPI.
static uintptr_t __irq_ack (uintptr_t en) {
	while (_xchg(&irqctrl_lock, 1));
	uintptr_t ret = hwdrvirqctrl_ack(_cpuid(), en);
	_xchg(&irqctrl_lock, 0);
	return ret;
}

bool __trap_irq (void) {
	_trap_savedctx_t *savedctx = _trap_savedctx();
	uintptr_t coreid = _cpuid();
	switch (savedctx->cause) {
		case (7 /* Machine Timer */ | (1<<(__riscv_xlen-1))):
			_timer_t *t;
			while (t = __timer_list[coreid]) {
				if (_clkcycles() >= t->e) {
					// Unlink an expired _timer, before executing it.
					if (t->l.next != &t->l) {
						__timer_list[coreid] = container_of(t->l.next, _timer_t, l);
						__settimecmp(__timer_list[coreid]->e);
						_dlist_del(t->l.prev, t->l.next);
					} else
						__timer_list[coreid] = 0;
					_dlist_clr(&t->l);
					t->f(t);
				} else
					break;
			}
			if (!__timer_list[coreid])
				__asm__ __volatile__ ("csrc mie, %0\n" :: "r"(0x80) : "memory"); // Clear mie.mtie.
			break;
		case (11 /* Machine External */ | (1<<(__riscv_xlen-1))):
			while (_xchg(&__irqs.lock, 1));
			uintptr_t irqsrc = __irq_ack(1);
			if (irqsrc != -2) {
				_irq_t *i = __irqs.l;
				if (i) while (1) {
					if (irqsrc == i->n) {
						_xchg(&__irqs.lock, 0);
						i->f(i);
						while (_xchg(&__irqs.lock, 1));
						if (!i->l.next)
							break;
					}
					i = container_of(i->l.next, _irq_t, l);
					if (i == __irqs.l)
						break;
				}
			}
			_xchg(&__irqs.lock, 0);
			break;
	}
	return false;
}

uintptr_t _mutex_lock (_mutex_t *m, _date_t timeout) {
	_preempt_disable();
	if (timeout) {
		while (_xchg(&m->lock, 1));
		if (m->acqcnt) {
			if (!m->waitq.l)
				_atomic_inc(&m->waitq.p);
			_xchg(&m->lock, 0);
			// m->waitq.p gets incremented to avoid a race condition
			// after unlocking m->lock with m->acqcnt becoming null,
			// when _thread_schedone(&m->waitq) is called by another thread
			// _mutex_unlock() on another CPU while current thread is here
			// and has not yet put itself on the wait-queue; resulting in
			// current thread missing its wake-up call and never waking up.
			_thread_sleeponwq(&m->waitq, timeout);
			while (_xchg(&m->lock, 1));
			if (m->acqcnt) {
				_xchg(&m->lock, 0);
				_preempt_enable();
				return 0;
			}
		}
		m->acqcnt = 1;
		_xchg(&m->lock, 0);
		_preempt_enable();
		return 1;
	}
	uintptr_t ret = 0;
	if (_xchg(&m->lock, 1))
		goto done;
	if (m->acqcnt)
		goto unlock;
	m->acqcnt = 1;
	ret = 1;
	unlock:
	_xchg(&m->lock, 0);
	done:
	_preempt_enable();
	return ret;
}

void _mutex_unlock (_mutex_t *m) {
	_preempt_disable();
	while (_xchg(&m->lock, 1));
	m->acqcnt = 0;
	_thread_schedone(&m->waitq);
	_xchg(&m->lock, 0);
	_preempt_enable();
}

uintptr_t _mutex_lock_recursive (_mutex_t *m, _date_t timeout) {
	if (m->owner == _tpval()) {
		if (m->acqcnt == -1)
			_oops();
		++m->acqcnt;
		return 1;
	}
	if (_mutex_lock(m, timeout)) {
		m->owner = _tpval();
		return 1;
	}
	return 0;
}

void _mutex_unlock_recursive (_mutex_t *m) {
	if (m->owner != _tpval())
		_oops();
	if (m->acqcnt > 1) {
		--m->acqcnt;
		return;
	}
	m->owner = 0;
	_mutex_unlock(m);
}

// Initialize a _fifo_t for use; if buf is null, the _fifo_t gets used as a semaphore.
void _fifo_init (_fifo_t *f, void *buf, size_t sz) {
	f->lock = 0;
	f->widx = 0;
	f->ridx = 0;
	f->buf = buf;
	f->sz = sz;
	f->wwaitq = (_waitq_t)_WAITQ_CLR;
	f->rwaitq = (_waitq_t)_WAITQ_CLR;
}

// Add data to a _fifo_t.
// The arguments buf and sz are respectively the data pointer and its size.
// The argument buf is ignored if null, or if the _fifo_t was _fifo_init() with buf null.
// This function sleeps until there is enough space in the _fifo_t.
// Null is returned if timeout occurred, otherwise the value of the argument sz is returned.
size_t _fifo_put (_fifo_t *f, void *buf, size_t sz, _date_t timeout) {
	_preempt_disable();
	size_t fsz = f->sz;
	if (sz > fsz)
		_oops();
	void put (void) {
		uintptr_t fwidx = f->widx;
		f->widx += sz; // Get adjusted by fsz when (f->ridx >= fsz) in _fifo_get().get().
		void *fbuf = f->buf;
		if (buf && fbuf) {
			if (fwidx >= fsz)
				fwidx -= fsz;
			size_t s = (fsz - fwidx);
			if (sz > s) {
				memcpy(fbuf + fwidx, buf, s);
				memcpy(fbuf, buf + s, (sz - s));
			} else
				memcpy(fbuf + fwidx, buf, sz);
		}
	}
	if (timeout) {
		while (_xchg(&f->lock, 1));
		if (((f->widx - f->ridx) + sz) > fsz) {
			if (!f->wwaitq.l)
				_atomic_inc(&f->wwaitq.p);
			_xchg(&f->lock, 0);
			// f->wwaitq.p gets incremented to avoid a race condition in a
			// similar manner that it is done and explained in _mutex_lock().
			_thread_sleeponwq(&f->wwaitq, timeout);
			while (_xchg(&f->lock, 1));
			if (((f->widx - f->ridx) + sz) > fsz) {
				_thread_schedone(&f->wwaitq);
				_xchg(&f->lock, 0);
				_preempt_enable();
				return 0; // Return 0 because it must be write-all or nothing.
			}
		}
		if (sz)
			put();
		_thread_schedone(&f->rwaitq);
		_xchg(&f->lock, 0);
		_preempt_enable();
		return sz;
	}
	size_t ret = 0;
	if (_xchg(&f->lock, 1))
		goto done;
	if (((f->widx - f->ridx) + sz) > fsz)
		goto unlock;
	if (sz)
		put();
	_thread_schedone(&f->rwaitq);
	ret = sz;
	unlock:
	_xchg(&f->lock, 0);
	done:
	_preempt_enable();
	return ret;
}

// Remove data from a _fifo_t.
// The arguments buf and sz are respectively the data destination pointer and its size.
// The argument buf is ignored if null, or if the _fifo_t was _fifo_init() with buf null.
// This function sleeps until there is enough data in the _fifo_t, unless (sz == -1), in
// which case whatever is available in the _fifo_t is removed returning the amount retrieved.
// If data can be retrieved from the _fifo_t, the argument peek prevents its removal if true.
// Null is returned if timeout occurred, otherwise the value of the argument sz is returned.
size_t _fifo_get (_fifo_t *f, void *buf, size_t sz, bool peek, _date_t timeout) {
	_preempt_disable();
	bool flush = (sz == -1);
	size_t fsz = f->sz;
	if (!flush && sz > fsz)
		_oops();
	void get (void) {
		uintptr_t fridx = f->ridx;
		if (!peek) {
			f->ridx += sz;
			if (f->ridx >= fsz) {
				// Adjust after modifying f->ridx.
				// (f->ridx <= f->widx) will always be true.
				f->ridx -= fsz;
				f->widx -= fsz;
			}
		}
		void *fbuf = f->buf;
		if (buf && fbuf) {
			if (fridx >= fsz)
				fridx -= fsz;
			size_t s = (fsz - fridx);
			if (sz > s) {
				memcpy(buf, fbuf + fridx, s);
				memcpy(buf + s, fbuf, (sz - s));
			} else
				memcpy(buf, fbuf + fridx, sz);
		}
	}
	if (flush || timeout) {
		while (_xchg(&f->lock, 1));
		if (flush)
			sz = (f->widx - f->ridx);
		else if ((f->widx - f->ridx) < sz) {
			if (!f->rwaitq.l)
				_atomic_inc(&f->rwaitq.p);
			_xchg(&f->lock, 0);
			// f->rwaitq.p gets incremented to avoid a race condition in a
			// similar manner that it is done and explained in _mutex_lock().
			_thread_sleeponwq(&f->rwaitq, timeout);
			while (_xchg(&f->lock, 1));
			if ((f->widx - f->ridx) < sz) {
				_thread_schedone(&f->rwaitq);
				_xchg(&f->lock, 0);
				_preempt_enable();
				return 0; // Return 0 because it must be read-all or nothing.
			}
		}
		if (sz)
			get();
		_thread_schedone(&f->wwaitq);
		_xchg(&f->lock, 0);
		_preempt_enable();
		return sz;
	}
	size_t ret = 0;
	if (_xchg(&f->lock, 1))
		goto done;
	if ((f->widx - f->ridx) < sz)
		goto unlock;
	if (sz)
		get();
	_thread_schedone(&f->wwaitq);
	ret = sz;
	unlock:
	_xchg(&f->lock, 0);
	done:
	_preempt_enable();
	return ret;
}

size_t _fifo_usage (_fifo_t *f) {
	_preempt_disable();
	while (_xchg(&f->lock, 1));
	size_t ret = (f->widx - f->ridx);
	_xchg(&f->lock, 0);
	_preempt_enable();
	return ret;
}

// Reset a _fifo_t empty, waking up any writers and readers.
void _fifo_rst (_fifo_t *f) {
	_preempt_disable();
	while (_xchg(&f->lock, 1));
	f->widx = 0;
	f->ridx = 0;
	// Wakeup all writers before readers to try avoiding waits.
	_thread_schedall(&f->wwaitq);
	_thread_schedall(&f->rwaitq);
	_xchg(&f->lock, 0);
	_preempt_enable();
}

volatile uintptr_t __ncpu = 0;
// Return number of CPUs that have booted.
uintptr_t _ncpu (void) {
	return __ncpu;
}

// Per CPU runqueue.
static struct {
	uintptr_t lock;
	_thread_t *l; // Points to a circular linked list of _thread_t;
	              // Points to the next _thread_t to own the cpu.
	volatile _thread_t *cur; // _thread_t currently owning the cpu.
	uintptr_t cnt; // Number of _thread_t in the circular linked list.
	_date_t scheddate; // Next scheduled preemption date when non-null.
	_timer_t schedlr; // Used for scheduled preemption of _thread_cur.
} __runq[NCPU] = {[0 ... NCPU-1] = {0, 0, 0, 0, 0, _TIMER_CLR}};

static uintptr_t schedlrhz[NCPU];

// Set the clock cycles it takes to schedule preempt all threads in a CPU.
void _schedlr_freq (uintptr_t cpu, uintptr_t cycles) {
	if (cpu >= __ncpu)
		_oops();
	schedlrhz[cpu] = cycles;
}

static void __thread_removefromwq (_thread_t *thrd) {
	while (_xchg(&thrd->wq->lock, 1));
	if (thrd->l.next != &thrd->l) {
		if (thrd == thrd->wq->l)
			thrd->wq->l = container_of(thrd->l.next, _thread_t, l);
		_dlist_del(thrd->l.prev, thrd->l.next);
	} else
		thrd->wq->l = 0;
	_xchg(&thrd->wq->lock, 0);
	thrd->wq = 0;
}

void __switchctx (_thread_t *to);

// Callback to wakeup a _thread_t put to sleep by _thread_sleep().
static void __thread_wakeup (_timer_t *t) {
	// IRQs are disabled since this function runs in a trap handling.
	_thread_t *thrd = container_of(t, _thread_t, z);
	if (thrd->wq) // If the thread is on a _waitq_t, it gets removed from it.
		__thread_removefromwq(thrd);
	uintptr_t cpu = thrd->cpu;
	if (cpu != _cpuid())
		_oops();
	while (_xchg(&__runq[cpu].lock, 1));
	_thread_t *curthrd = (_thread_t *)__runq[cpu].cur;
	if (curthrd) {
		_dlist_add(&thrd->l, curthrd->l.prev, &curthrd->l);
		__runq[cpu].l = curthrd;
	} else {
		_dlist_init(&thrd->l);
		__runq[cpu].l = thrd;
	}
	__runq[cpu].cnt += 1;
	thrd->state = _THREAD_RUNNING;
	__runq[cpu].cur = thrd;
	_xchg(&__runq[cpu].lock, 0);
	_date_t scheddate, curscheddate = __runq[cpu].scheddate;
	if (__runq[cpu].cnt > 1) {
		scheddate = (_clkcycles() + (schedlrhz[cpu] / __runq[cpu].cnt));
		_timer_arm(&__runq[cpu].schedlr, scheddate);
		__runq[cpu].scheddate = scheddate;
	} else
		__runq[cpu].scheddate = 0;
	if (curthrd && curscheddate && curscheddate < scheddate)
		curthrd->ts = (curscheddate - _trap_savedctx()->cycle);
	__switchctx(thrd);
}

static void __timer_preempt (_timer_t *);

// Callback for preempting _thread_cur when receiving an IPI.
void __ipi_preempt (_irq_t *) {
	__timer_preempt(0);
}

_irq_t __ipi;

// To be used only by _start().
void __init_multithreading (_thread_t *thrd) {
	_dlist_init(&thrd->l);
	thrd->state = _THREAD_RUNNING;
	thrd->wq = 0;
	_timer_init(&thrd->z, __thread_wakeup);
	thrd->ts = 0;
	thrd->stack = 0;
	thrd->cpu = 0;
	thrd->savedctx.tp = (uintptr_t)_tpval();
	_irq_init(&__ipi, -1, __ipi_preempt);
	_irq_register(&__ipi);
	__ncpu += 1;
	// Send IPIs to start the other CPUs.
	for (uintptr_t i = 1;; ++i) {
		if (i > NCPU) // TODO: To be removed once percpu support is complete.
			_oops();
		uintptr_t ncpu = __ncpu;
		if (__irq_ipi(i) == -1)
			break;
		while (ncpu == __ncpu); // Wait for CPU to increment __ncpu.
	}
	// TODO: Initialize percpu data here before using them below.
	// TODO: The number of CPUs needs to be determined here early
	// TODO: and used to allocated just enough percpu data.
	__runq[0].l = thrd;
	__runq[0].cur = thrd;
	__runq[0].cnt = 1;
	for (uintptr_t i = 0; i < __ncpu; ++i) {
		_timer_init(&__runq[i].schedlr, __timer_preempt);
		schedlrhz[i] = SCHEDLRHZ;
	}
}

// To be used only by _start().
// Cannot use percpu data, as they have not yet been setup.
void __init_secondary_cpu (void) {
	__irq_ack(1);
}

// Create a new _thread_t and initialize its TLS and _thread_t areas.
// The newly created _thread_t state is initially _THREAD_STOPPED
// until used with _thread_sched() or _thread_schedoncpu().
// If stack is null, it gets allocated using stacksz.
_thread_t *_thread_create (void* stack, uintptr_t stacksz, void (*entry)(void *arg), void *arg) {
	bool is_stack_given = (stack ? true : false);
	if (!is_stack_given) {
		stack = malloc(stacksz);
		if (!stack)
			_oops();
		stacksz = malloc_usable_size(stack);
	}
	_thread_t *thrd = ((stack + stacksz) - sizeof(_thread_t));
	extern char __tdata_start[], __tdata_end[], __tbss_start[], __tbss_end[];
	void* tp = ((void *)thrd - (__tbss_end - __tdata_start));
	uintptr_t tdatasz = (__tdata_end - __tdata_start);
	memcpy (tp, __tdata_start, tdatasz);
	memset ((tp + tdatasz), 0, (__tbss_end - __tbss_start));
	*(_thread_t **)tp = thrd;
	_dlist_clr(&thrd->l);
	thrd->state = _THREAD_STOPPED;
	thrd->wq = 0;
	_timer_init(&thrd->z, __thread_wakeup);
	thrd->ts = 0;
	thrd->stack = (is_stack_given ? 0 : stack);
	thrd->cpu = -(_cpuid() + 1); // Negate to signal __switchctx().
	thrd->savedctx.ra = (uintptr_t)_thread_exit;
	thrd->savedctx.sp = (uintptr_t)tp;
	thrd->savedctx.tp = (uintptr_t)tp;
	thrd->savedctx.s0 = (uintptr_t)arg;
	thrd->savedctx.s1 = (uintptr_t)entry;
	thrd->savedctx.scratch = 0;
	thrd->savedctx.status = 0x8; // Set mstatus.mie to have IRQs initially enabled.
	return thrd;
}

// Move a thread to a cpu.
// If the thread is on a _waitq_t, it gets removed from it.
// The argument pin, when true, prevents load-balancing from migrating the thread.
// The thread being moved cannot be _thread_cur.
// Note that it does not preempt _thread_cur.
void _thread_schedoncpu (_thread_t *thrd, uintptr_t cpu, bool pin) {
	_preempt_disable();
	// _thread_cur is not used in this function, otherwise
	// it would not be useable in a trap handling.
	// The thread being moved cannot be _thread_cur (ie: __runq[_cpuid()].cur),
	// because it needs its resume context to already have been saved.
	if (thrd == __runq[_cpuid()].cur || !thrd->savedctx.tp || cpu >= __ncpu)
		_oops();
	if (thrd->wq)
		__thread_removefromwq(thrd);
	// If thrd state is already _THREAD_RUNNING, remove it from its runq.
	if (thrd->state == _THREAD_RUNNING) {
		uintptr_t cpu = thrd->cpu; // This scope has its own variable `cpu`.
		// If thrd went through a previous call of _thread_sched() without
		// going through __switchctx(), its field cpu could still be negative.
		if ((intptr_t)cpu < 0)
			cpu = ((-cpu) - 1);
		while (_xchg(&__runq[cpu].lock, 1));
		uintptr_t is_oncpu = (thrd == __runq[cpu].cur); // Is running on its cpu.
		if (thrd->l.next != &thrd->l) {
			if (thrd == __runq[cpu].l)
				__runq[cpu].l = container_of(thrd->l.next, _thread_t, l);
			_dlist_del(thrd->l.prev, thrd->l.next);
		} else
			__runq[cpu].l = 0;
		__runq[cpu].cnt -= 1;
		_xchg(&__runq[cpu].lock, 0);
		if (cpu != _cpuid() && is_oncpu) {
			// Send IPI and spinwait until thrd is no longer running on the CPU.
			__irq_ipi(cpu);
			while (thrd == __runq[cpu].cur);
		}
	}
	if ((intptr_t)thrd->cpu < 0)
		thrd->cpu = -(cpu + 1); // Negate to signal __switchctx().
	else
		thrd->cpu = cpu;
	while (_xchg(&__runq[cpu].lock, 1));
	if (__runq[cpu].l)
		_dlist_add(&thrd->l, __runq[cpu].l->l.prev, &__runq[cpu].l->l);
	else
		_dlist_init(&thrd->l);
	__runq[cpu].l = thrd;
	__runq[cpu].cnt += 1;
	thrd->state = _THREAD_RUNNING;
	_xchg(&__runq[cpu].lock, 0);
	if (cpu != _cpuid() && !__runq[cpu].cur) // Send IPI if cpu halted.
		__irq_ipi(cpu);
	_preempt_enable();
}

// Schedule a thread to run next on its cpu (ie: thrd->cpu).
// If the thread is on a _waitq_t, it gets removed from it.
// Note that it does not preempt _thread_cur.
void _thread_sched (_thread_t *thrd) {
	uintptr_t cpu = thrd->cpu;
	if ((intptr_t)cpu < 0)
		cpu = ((-cpu) - 1);
	_thread_schedoncpu(thrd, cpu, false);
}

// Force-stop a thread if it is running.
// Note that it does not preempt _thread_cur.
// _thread_sched() or _thread_schedoncpu() can resume the thread.
void _thread_stop (_thread_t *thrd) {
	_preempt_disable();
	if (thrd->wq)
		__thread_removefromwq(thrd);
	// If thrd state is already _THREAD_RUNNING, remove it from its runq.
	if (thrd->state == _THREAD_RUNNING) {
		uintptr_t cpu = thrd->cpu;
		while (_xchg(&__runq[cpu].lock, 1));
		uintptr_t is_oncpu = (thrd == __runq[cpu].cur); // Is running on its cpu.
		if (thrd->l.next != &thrd->l) {
			if (thrd == __runq[cpu].l)
				__runq[cpu].l = container_of(thrd->l.next, _thread_t, l);
			_dlist_del(thrd->l.prev, thrd->l.next);
		} else
			__runq[cpu].l = 0;
		__runq[cpu].cnt -= 1;
		_xchg(&__runq[cpu].lock, 0);
		if (cpu != _cpuid() && is_oncpu) {
			// Send IPI and spinwait until thrd is no longer running on the CPU.
			__irq_ipi(cpu);
			while (thrd == __runq[cpu].cur);
		}
	}
	thrd->state = _THREAD_STOPPED;
	_dlist_clr(&thrd->l);
	_preempt_enable();
}

// Terminate a thread by calling _thread_stop() on it and preparing it for _thread_dispose().
// _thread_sched() or _thread_schedoncpu() can no longer resume the thread.
void _thread_kill (_thread_t *thrd) {
	_thread_stop(thrd);
	thrd->savedctx.tp = 0;
}

// Free memory used by a terminated _thread_t returned by _thread_create().
void _thread_dispose (_thread_t *thrd) {
	if (!_is_thread_terminated(thrd))
		_oops();
	if (thrd->stack)
		free(thrd->stack);
}

// Sleep until the expiration date given as argument.
// If the expiration date given is -1, _thread_cur sleeps
// indefinitely until it is woken up by _thread_sched().
// Put _thread_cur on _waitq_t if wq is non-null.
void _thread_sleeponwquntil (_waitq_t *wq, _date_t e) {
	_preempt_disable();
	_thread_t *nxtthrd, *thrd = _thread_cur;
	if (e != _DATE_MAX)
		_timer_arm(&thrd->z, e);
	uintptr_t cpu = thrd->cpu;
	if (cpu != _cpuid())
		_oops();
	while (_xchg(&__runq[cpu].lock, 1));
	if (thrd->l.next != &thrd->l) {
		if (thrd == __runq[cpu].l)
			_oops(); // __runq[cpu].l should be pointing to the next _thread_t and not _thread_cur.
		nxtthrd = __runq[cpu].l;
		_dlist_del(thrd->l.prev, thrd->l.next);
		__runq[cpu].l = container_of(nxtthrd->l.next, _thread_t, l);
	} else {
		if (thrd != __runq[cpu].l)
			_oops();
		nxtthrd = 0;
		__runq[cpu].l = 0;
	}
	__runq[cpu].cnt -= 1;
	__runq[cpu].cur = nxtthrd;
	_xchg(&__runq[cpu].lock, 0);
	thrd->state = _THREAD_STOPPED;
	if (wq) {
		while (_xchg(&wq->lock, 1));
		if (wq->l)
			_dlist_add(&thrd->l, ((_thread_t *)wq->l)->l.prev, &((_thread_t *)wq->l)->l);
		else {
			_dlist_init(&thrd->l);
			wq->l = thrd;
		}
		_xchg(&wq->lock, 0);
		thrd->wq = wq;
	} else
		_dlist_clr(&thrd->l);
	if (__runq[cpu].cnt > 1) {
		_date_t scheddate = _clkcycles();
		if (nxtthrd->ts) {
			scheddate += nxtthrd->ts;
			nxtthrd->ts = 0;
		} else
			scheddate += (schedlrhz[cpu] / __runq[cpu].cnt);
		_timer_arm(&__runq[cpu].schedlr, scheddate);
		__runq[cpu].scheddate = scheddate;
	} else
		__runq[cpu].scheddate = 0;
	__switchctx(nxtthrd); // Will halt if nxtthrd is null.
	_preempt_enable();
}

// Call _thread_sched() on the thread that was first added to the waitq.
// Note that it does not preempt _thread_cur.
void _thread_schedone (_waitq_t *wq) {
	if (wq->p) { // Avoid a race condition until wq->l is true.
		while (!wq->l)
			asm volatile("" ::: "memory");
		_atomic_dec(&wq->p);
		goto wq_l_true;
	}
	if (wq->l) {
		wq_l_true:
		// _thread_sched() removes the _thread_t from the _waitq_t.
		_thread_sched(wq->l);
	}
}

// Call _thread_sched() on all threads in the waitq,
// starting with the thread that was first added to the waitq.
// Note that it does not preempt _thread_cur.
void _thread_schedall (_waitq_t *wq) {
	_preempt_disable();
	if (wq->p) { // Avoid a race condition until wq->l is true.
		while (!wq->l)
			asm volatile("" ::: "memory");
		_atomic_dec(&wq->p);
		goto wq_l_true;
	}
	while (wq->l) {
		wq_l_true:
		// _thread_sched() removes the _thread_t from the _waitq_t.
		_thread_sched(wq->l);
	}
	_preempt_enable();
}

// Preempt thread currently running on a cpu.
void _thread_preempt (uintptr_t cpu) {
	_preempt_disable();
	if (cpu >= __ncpu)
		_oops();
	if (cpu != _cpuid()) {
		// Send IPI to preempt thread running on the CPU.
		__irq_ipi(cpu);
		// No need to wait, because it could still
		// be the same thread running on the CPU.
		_preempt_enable();
		return;
	}
	while (_xchg(&__runq[cpu].lock, 1));
	_thread_t *nxtthrd = __runq[cpu].l;
	if (!nxtthrd) {
		__runq[cpu].cur = 0;
		_xchg(&__runq[cpu].lock, 0);
		__switchctx(0); // Will halt.
		goto done;
	}
	__runq[cpu].l = container_of(nxtthrd->l.next, _thread_t, l);
	_thread_t *curthrd = (_thread_t *)__runq[cpu].cur;
	__runq[cpu].cur = nxtthrd;
	_xchg(&__runq[cpu].lock, 0);
	if (nxtthrd != curthrd) {
		if (__runq[cpu].cnt > 1) {
			_date_t scheddate = _clkcycles();
			if (nxtthrd->ts) {
				scheddate += nxtthrd->ts;
				nxtthrd->ts = 0;
			} else
				scheddate += (schedlrhz[cpu] / __runq[cpu].cnt);
			_timer_arm(&__runq[cpu].schedlr, scheddate);
			__runq[cpu].scheddate = scheddate;
		} else
			__runq[cpu].scheddate = 0;
		__switchctx(nxtthrd);
	} else
		__runq[cpu].scheddate = 0;
	done:
	_preempt_enable();
}

// Callback for scheduled preemption of _thread_cur.
static void __timer_preempt (_timer_t *) {
	// IRQs are disabled since this function runs in a trap handling.
	uintptr_t cpu = _cpuid();
	while (_xchg(&__runq[cpu].lock, 1));
	_thread_t *nxtthrd = __runq[cpu].l;
	if (!nxtthrd) {
		__runq[cpu].cur = 0;
		_xchg(&__runq[cpu].lock, 0);
		__switchctx(0); // Will halt.
		goto done;
	}
	__runq[cpu].l = container_of(nxtthrd->l.next, _thread_t, l);
	__runq[cpu].cur = nxtthrd;
	_xchg(&__runq[cpu].lock, 0);
	if (!_tpval() || nxtthrd != _thread_cur) {
		if (__runq[cpu].cnt > 1) {
			_date_t scheddate = _clkcycles();
			if (nxtthrd->ts) {
				scheddate += nxtthrd->ts;
				nxtthrd->ts = 0;
			} else
				scheddate += (schedlrhz[cpu] / __runq[cpu].cnt);
			_timer_arm(&__runq[cpu].schedlr, scheddate);
			__runq[cpu].scheddate = scheddate;
		} else
			__runq[cpu].scheddate = 0;
		__switchctx(nxtthrd);
	} else
		__runq[cpu].scheddate = 0;
	done:;
}

// Terminate _thread_cur by calling _thread_stop() on it and preparing it for _thread_dispose().
// _thread_sched() or _thread_schedoncpu() can no longer resume the thread.
void _thread_exit (void) {
	_thread_kill(_thread_cur);
	_thread_yield();
}

// unimplemented exceptions.
bool __trap_exc_insn_misaligned (void) { while(1); }
bool __trap_exc_insn_afault (void) { while(1); }
bool __trap_exc_insn_illegal (void) { while(1); }
bool __trap_exc_break (void) { while(1); }
bool __trap_exc_load_misaligned (void) { while(1); }
bool __trap_exc_load_afault (void) { while(1); }
bool __trap_exc_store_misaligned (void) { while(1); }
bool __trap_exc_store_afault (void) { while(1); }
bool __trap_exc_ecall_u (void) { while(1); }
bool __trap_exc_ecall_s (void) { while(1); }
bool __trap_exc_insn_pfault (void) { while(1); }
bool __trap_exc_load_pfault (void) { while(1); }
bool __trap_exc_store_pfault (void) { while(1); }
bool __trap_exc_inv (void) { while(1); }
