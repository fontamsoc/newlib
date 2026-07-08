// SPDX-License-Identifier: GPL-2.0-only
// 20260706 (c) William Fonkou Tambe

// Written from https://github.com/mborgerson/gdbstub

// GDB Remote-Serial-Protocol stub complementing underLineOS.
//
// This file gets built in its own archive libgdbstub.a so that the stub
// is part of the final binary only when an application links -lgdbstub;
// it then overrides the weak trap handlers __trap_exc_break and friends
// from _os.c (which crt0.o references from its exception dispatch table,
// insuring this object gets extracted from the archive), and registers
// an _irq for its serial device so that a host gdb can attach to the
// running application at any time, as well as interrupt it (ie: ^C).
//
// The stub debugs individual threads: it runs in its own ENGINE thread
// which services gdb packets while the application keeps running, and
// the thread being debugged is the one pointed by the strong global
// _thread_t *__gdbstub_tp (the SELECTION), which gdb switches using:
//	set var __gdbstub_tp = <thread>
// The g/G/p/P packets serve the selection registers from its saved
// context, and the c/s packets resume/step only the selection; ie: the
// wire protocol is unchanged, the whole thread-selection user-interface
// is that variable and the two functions below.
//
// A thread hitting an ebreak (ie: a gdb breakpoint) or faulting parks
// itself and queues a stop event; the stub reports a stop-reply only
// for the selection own stops, every other thread event stays queued
// until drained, at the gdb prompt, using:
//	set var __gdbstub_tp = __gdbstub_next()
// which resumes the previously selected thread and adopts the oldest
// queued stopped thread (or the debug-shell thread when none), while
//	set var __gdbstub_tp = __gdbstub_last()
// selects back the thread most recently resumed. The parked threads
// are visible from gdb with: print __gdbstub_parked .
//
// ^C parks the selection if it is running, and never changes the
// selection. At boot the selection is the DEBUG-SHELL thread, a stub
// thread resting on an ebreak with a full register frame: a stable
// context which perturbs nothing when inspected, and on which gdb
// inferior calls (ie: the __gdbstub_next() idiom above) are hosted.
//
// Since gdb removes its breakpoints from memory whenever it stops
// while other threads keep running, using the following gdb setting
// is recommended: set breakpoint always-inserted on .
//
// Faults from contexts which cannot park (a nested trap such as from
// _oops(), the idle context, the engine thread itself, or when the
// parked list is full) drop in an EMERGENCY session which freezes the
// whole machine and services gdb synchronously from the trap, as
// followed by the v1 stub for every stop.
//
// The debugging transport is by default the serial_pty0 device mapped
// at 0xe80 (irqctrl source 1); an application can retarget it defining
// strong versions of the weak configuration globals _gdbstub_dev and
// _gdbstub_irq; similarly _gdbstub_membeg/_gdbstub_memend bound the
// memory that gdb is allowed to access (accessing an unmapped address
// terminates the simulation, hence out-of-bounds requests are refused).
//
// Software breakpoints (gdb writes an ebreak using the M packet) and
// single-stepping (the stub plants temporary ebreak at the successor
// instructions) modify code memory; the dcache being write-back while
// fence.i only invalidates the icache, each modified word is pushed to
// the memory-side coherency point using an atomic access (which bypasses
// the dcache) so that the icache refill observes it; see gdbstub_memsync().

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "_os.h"

#include <machine/hwdrvchar.h>
#include <machine/hwdrvirqctrl.h>

// Configuration; weak so that an application can override them.
__attribute__((weak)) void *_gdbstub_dev = (void *)0xe80;
__attribute__((weak)) uintptr_t _gdbstub_irq = 1;
__attribute__((weak)) uintptr_t _gdbstub_membeg = 0x1000;
__attribute__((weak)) uintptr_t _gdbstub_memend = 0; // Null selects roundup(__heap_end, 1KB).

// Referenced by _os.c to detect that the gdbstub is linked-in.
char _gdbstub_active = 1;

// ****************************************************************************
// State
// ****************************************************************************

// Signal numbers reported to gdb.
#define GDBSTUB_SIGINT  2
#define GDBSTUB_SIGILL  4
#define GDBSTUB_SIGTRAP 5
#define GDBSTUB_SIGBUS  10
#define GDBSTUB_SIGSEGV 11

#define GDBSTUB_STEP_MAX 4

// The selection: the thread whose registers g/G/p/P serve and which
// c/s/^C act on; gdb switches it with: set var __gdbstub_tp = <thread> .
// The stub never writes null in it (a bogus value, including null, makes
// the registers read unavailable and c/s no-op until gdb corrects it).
_thread_t *__gdbstub_tp = 0;

// Classification of the selection saved context, which decides the
// register set that g/G/p/P can serve; see gdbstub_ctx_class().
#define GDBSTUB_CTX_INVALID 0 // Not a valid thread pointer.
#define GDBSTUB_CTX_DEAD    1 // Terminated thread.
#define GDBSTUB_CTX_LIVE    2 // On-CPU or mid-switch; registers unavailable.
#define GDBSTUB_CTX_FRESH   3 // Created but never run.
#define GDBSTUB_CTX_FULL    4 // At rest with a full trap frame.
#define GDBSTUB_CTX_COOP    5 // Switched-out cooperatively; callee-saved only.

// Parked threads bookkeeping; a thread is stub-parked (ie: stopped by
// the stub, resumable by it) while it has an entry here; the entries
// are kept compact in arrival order, hence the oldest first. Non-static
// so that gdb can list the stopped threads with: print __gdbstub_parked .
#define GDBSTUB_PARKED_EVT        1 // Stop event not yet reported/drained.
#define GDBSTUB_PARKED_SUPPRESSED 2 // Held while another thread single-steps.
#define GDBSTUB_PARKED_MAX 16
typedef struct {
	_thread_t *thrd;
	unsigned char sig;
	unsigned char flags;
} __gdbstub_parked_t;
__gdbstub_parked_t __gdbstub_parked[GDBSTUB_PARKED_MAX];
uintptr_t __gdbstub_parked_cnt = 0;

// Single static instance; buffers must not live on the
// stack as parts of the stub run on trap or small stacks.
static struct {
	uintptr_t signum;      // Last stop signal; what the ? packet reports.
	uintptr_t regs[33];    // Staging, per gdb register order: x0-x31, pc.
	uint64_t avail;        // Which staged registers are available; bit per register.
	bool resumed;          // Set when gdb is awaiting a stop-reply (ie: after c/s).
	uintptr_t membeg;      // Memory bounds gdb is allowed to access.
	uintptr_t memend;
	uintptr_t lock;        // Serializes the wire between the engine and emergency sessions.
	void *lock_owner;      // Owner cookie backing the bounded-spin steal of the wire lock.
	unsigned wire_epoch;   // Incremented by each emergency session, whose byte
	                       // consumption invalidates any in-flight engine exchange.
	unsigned step_cnt;     // Temporary breakpoints planted by a single-step.
	uintptr_t step_addr[GDBSTUB_STEP_MAX];
	uint32_t step_orig[GDBSTUB_STEP_MAX];
	_thread_t *step_owner; // Thread being single-stepped; null when none.
	uintptr_t plock;       // Serializes __gdbstub_parked[] and the fields below.
	_thread_t *resume_pending; // Recorded by __gdbstub_next(); resumed by the engine.
	_thread_t *last_resumed[2]; // Resume history backing __gdbstub_last(); two
	                            // deep, as the inferior call of the idiom itself
	                            // resumes the selection hosting it.
	_waitq_t wq;           // Where the engine thread sleeps awaiting bytes/events.
	_thread_t *engine;     // The engine thread servicing gdb packets.
	_thread_t *shell;      // The debug-shell thread.
	_thread_t *mainthrd;   // The initial thread; its _thread_t lives above __heap_end.
	char pkt_buf[1024];    // Packet buffer; qSupported reports (sizeof-8) as PacketSize.
	char mem_buf[512];     // m/M packets data staging buffer.
} gdbstub = {.signum = GDBSTUB_SIGTRAP};

// Forward declarations for the few call cycles of this file.
static int gdbstub_getc_wait (void);
static void gdbstub_ctrlc (void);
static void gdbstub_step_lift (void);
static void gdbstub_emergency (uintptr_t signum);

// ****************************************************************************
// Serial device accessors
// ****************************************************************************

// hwdrvchar_t instance used with <machine/hwdrvchar.h>; its field addr
// gets loaded from the weak _gdbstub_dev at each use; hwdrvchar_init()
// is not needed as only hwdrvchar_readable() and hwdrvchar_interrupt()
// are used, which do not depend on the fields it fills.
static hwdrvchar_t gdbstub_hwdrvchar;

// Read a byte from the serial device; it blocks
// the bus until a byte is available, hence thread
// context callers must first insure availability
// using gdbstub_rxusage() so that the application
// is not frozen awaiting gdb bytes.
static int gdbstub_getc (void) {
	return *(volatile unsigned char *)_gdbstub_dev;
}

// Write a byte to the serial device; it never blocks.
static void gdbstub_putc (int c) {
	*(volatile unsigned char *)_gdbstub_dev = c;
}

// Return the count of bytes that can be read without blocking.
static uintptr_t gdbstub_rxusage (void) {
	gdbstub_hwdrvchar.addr = _gdbstub_dev;
	return hwdrvchar_readable(&gdbstub_hwdrvchar);
}

// Configure the serial device interrupt; the argument threshold is the
// receive buffer byte amount that triggers an interrupt, null disables.
// The device disables its interrupt when acknowledged, hence this must
// be called again after each interrupt service.
static void gdbstub_setintr (uintptr_t threshold) {
	gdbstub_hwdrvchar.addr = _gdbstub_dev;
	hwdrvchar_interrupt(&gdbstub_hwdrvchar, threshold);
}

// Force the serial device back to command-ready, discarding any stale
// command reply; an emergency session may have frozen another context
// mid device-command handshake (the accessors are two-write sequences
// which the device gates on the previous command being completed),
// which would otherwise make every accessor above wait forever.
static void gdbstub_dev_recover (void) {
	(void)_xchg((uintptr_t *)(_gdbstub_dev + sizeof(uintptr_t)),
		(uintptr_t)HWDRVCHAR_CMDDEVRDY);
}

// Read a byte of an in-flight exchange (the bytes following a
// packet-start, and acknowledgments), where recursing in the event
// pump (ie: gdbstub_getc_wait()) must be avoided. Spin-polls briefly
// so that a streaming packet is consumed at full speed with no
// interrupt overhead, then sleeps until the serial interrupt signals
// the next byte, which keeps the reception correct at ANY transport
// pace (an arbitrarily large SERIAL_PTY_POLLCYCLES in rv32-sim, or an
// arbitrarily slow real UART): yielding instead would cost a scheduler
// rotation PER BYTE, making long packets outlast gdb retransmission
// timeout and wedging the session.
// The availability check and the bus-blocking read are done with
// preemption disabled: an emergency session entered from a thread
// scheduled in-between would otherwise consume the checked bytes,
// leaving the read to stall the whole machine on an empty buffer.
// Returns -1 when gdbstub.wire_epoch no longer matches the epoch
// argument: an emergency session ran and consumed the remainder of
// whatever exchange the caller was in the middle of, which must be
// abandoned (awaiting its bytes would wait forever); the timeout on
// the sleep below also bounds how late that gets detected.
// Within a trap (ie: an emergency session), it degrades to the
// blocking read as there is no thread machinery to wait with.
static int gdbstub_getc_poll (unsigned epoch) {
	if (_trap_savedctx())
		return gdbstub_getc();
	while (1) {
		for (unsigned spin = 2048; spin; --spin) {
			if (gdbstub.wire_epoch != epoch)
				return -1;
			_preempt_disable();
			if (gdbstub_rxusage()) {
				int c = gdbstub_getc();
				_preempt_enable();
				return c;
			}
			_preempt_enable();
		}
		// Level interrupt: arm before the final check, as in
		// gdbstub_getc_wait(), so that a byte landing in-between
		// pends the interrupt across the sleep instead of being
		// missed; the interrupt handler wakes the engine, which
		// resumes the spin above.
		gdbstub_setintr(1);
		_preempt_disable();
		if (!gdbstub_rxusage() && gdbstub.wire_epoch == epoch)
			_thread_sleeponwquntil(&gdbstub.wq, (_clkcycles() + _MSECS(50)));
		_preempt_enable();
	}
}

// ****************************************************************************
// Code/Data memory coherency
// ****************************************************************************

// Push to memory the dcache view of the word range [addr, addr+len);
// atomically writing back the value just plain-loaded goes around the
// write-back dcache straight to the memory-side coherency point, which
// is where the icache refills from; to be used after modifying code,
// followed by gdbstub_fencei() which invalidates the icache.
static void gdbstub_memsync (uintptr_t addr, uintptr_t len) {
	uintptr_t end = (addr + len);
	for (addr &= ~(uintptr_t)3; addr < end; addr += sizeof(uintptr_t)) {
		uintptr_t val = *(volatile uintptr_t *)addr;
		(void)_xchg((uintptr_t *)addr, val);
	}
}

static void gdbstub_fencei (void) {
	__asm__ __volatile__ ("fence.i\n" ::: "memory");
}

// Return whether gdb is allowed to access [addr, addr+len).
static bool gdbstub_memok (uintptr_t addr, uintptr_t len) {
	return (addr >= gdbstub.membeg && addr < gdbstub.memend &&
		len <= (gdbstub.memend - addr));
}

// Initialize the memory bounds; done by the constructor, and lazily by
// an emergency session so that pre-constructor faults are serviceable.
static void gdbstub_bounds_init (void) {
	if (gdbstub.memend)
		return;
	gdbstub.membeg = _gdbstub_membeg;
	if (_gdbstub_memend)
		gdbstub.memend = _gdbstub_memend;
	else {
		// Round up to 1KB; the RAM top is 1KB aligned and __heap_end
		// is at most a small carve-out (TLS and main _thread_t) below
		// it, hence this can never overshoot the RAM top.
		extern uintptr_t __heap_end;
		gdbstub.memend = ((__heap_end + 1023) & ~(uintptr_t)1023);
	}
}

// ****************************************************************************
// Hex encoding/decoding
// ****************************************************************************

// Get the corresponding ASCII hex digit character for a value.
static char gdbstub_digit (unsigned val) {
	static const char digits[] = "0123456789abcdef";
	return digits[val&0xf];
}

// Get the corresponding value for an ASCII hex digit character.
// Returns -1 if not a hex digit.
static int gdbstub_val (char digit) {
	if ((digit >= '0') && (digit <= '9'))
		return (digit - '0');
	else if ((digit >= 'a') && (digit <= 'f'))
		return ((digit - 'a') + 0xa);
	else if ((digit >= 'A') && (digit <= 'F'))
		return ((digit - 'A') + 0xa);
	else
		return -1;
}

// Encode data to its hex-value representation in a buffer.
// Returns the number of characters written to buf, or -1
// if the buffer is too small.
static int gdbstub_enc_hex (
	char *buf, unsigned buf_len, char *data, unsigned data_len) {
	if (buf_len < data_len*2) // Buffer too small.
		return -1;
	for (unsigned pos = 0; pos < data_len; ++pos) {
		*buf++ = gdbstub_digit((data[pos] >> 4) & 0xf);
		*buf++ = gdbstub_digit((data[pos]     ) & 0xf);
	}
	return data_len*2;
}

// Decode data from its hex-value representation in a buffer.
// Returns 0 if successful, -1 if the buffer does not have
// the exact expected size or contains a non-hex character.
static int gdbstub_dec_hex (
	char *buf, unsigned buf_len, char *data, unsigned data_len) {
	if (buf_len != data_len*2)
		return -1;
	for (unsigned pos = 0; pos < data_len; ++pos) {
		int tmp = gdbstub_val(*buf++); // Decode high nibble.
		if (tmp == -1)
			return -1;
		data[pos] = (tmp << 4);
		tmp = gdbstub_val(*buf++); // Decode low nibble.
		if (tmp == -1)
			return -1;
		data[pos] |= tmp;
	}
	return 0;
}

// Parse an unsigned hex integer advancing *pp within [*pp, end).
// Returns the count of digits consumed.
static unsigned gdbstub_parsehex (char **pp, char *end, uintptr_t *out) {
	uintptr_t val = 0;
	unsigned cnt = 0;
	while (*pp < end) {
		int d = gdbstub_val(**pp);
		if (d == -1)
			break;
		val = ((val << 4) | d);
		++*pp; ++cnt;
	}
	*out = val;
	return cnt;
}

// ****************************************************************************
// Wire lock
// ****************************************************************************

// The wire lock serializes transmissions between the engine thread and
// emergency sessions (and between CPUs); an emergency session freezes
// the machine while the engine could be holding the lock mid-packet,
// hence acquiring is a bounded spin after which the lock is STOLEN
// (the frozen owner release then does nothing); the owner cookie also
// makes re-acquiring by the current owner (ie: each transmission of an
// emergency session already holding the lock) a no-op.

static void gdbstub_wire_acquire (void *owner) {
	// The re-acquire test keys on the owner cookie alone: it is
	// plain-written hence coherently plain-readable, while the lock
	// word only ever holds its value at the AMO coherency point
	// (a plain read of it would see a stale dcache line).
	if (gdbstub.lock_owner == owner)
		return; // Already owned.
	uintptr_t spin = (1 << 22);
	while (_xchg(&gdbstub.lock, 1)) {
		if (!--spin)
			break; // Steal from the frozen owner.
	}
	gdbstub.lock_owner = owner;
}

static void gdbstub_wire_release (void *owner) {
	if (gdbstub.lock_owner != owner)
		return; // Was stolen; the thief now owns it.
	gdbstub.lock_owner = 0;
	_xchg(&gdbstub.lock, 0);
}

// Wire lock owner cookie for the current context.
static void *gdbstub_wire_self (void) {
	_savedctx_t *f = _trap_savedctx();
	return (f ? (void *)f : (void *)_thread_cur);
}

// ****************************************************************************
// Packet layer
// ****************************************************************************

// Calculate the 8-bit checksum of a buffer.
static unsigned char gdbstub_checksum (char *buf, unsigned len) {
	unsigned char csum = 0;
	while (len) {
		csum += *buf++;
		len -= 1;
	}
	return csum;
}

// Transmit a packet of data of the form: $<packet-data>#<checksum> ;
// retransmits until gdb acknowledges it, or until an emergency session
// takes over the wire (it then owns the dialogue; the pending
// acknowledgment is abandoned).
static void gdbstub_send_packet (char *pkt_data, unsigned pkt_len) {
	void *self = gdbstub_wire_self();
	unsigned epoch = gdbstub.wire_epoch;
	gdbstub_wire_acquire(self);
	while (1) {
		gdbstub_putc('$');
		for (unsigned i = 0; i < pkt_len; ++i)
			gdbstub_putc(pkt_data[i]);
		unsigned char csum = gdbstub_checksum(pkt_data, pkt_len);
		gdbstub_putc('#');
		gdbstub_putc(gdbstub_digit(csum >> 4));
		gdbstub_putc(gdbstub_digit(csum & 0xf));
		int c;
		do { // Wait for the acknowledgment.
			c = gdbstub_getc_poll(epoch);
		} while (c != -1 && c != '+' && c != '-');
		if (c != '-')
			break;
	}
	gdbstub_wire_release(self);
}

// Transmit a null-terminated string as a packet.
static void gdbstub_send_str (char *s) {
	unsigned len = 0;
	while (s[len])
		len += 1;
	gdbstub_send_packet(s, len);
}

// Transmit a stop-reply packet (ie: S05) for the signal given as argument.
static void gdbstub_send_signal (uintptr_t signum) {
	char buf[3] = {'S', gdbstub_digit(signum >> 4), gdbstub_digit(signum & 0xf)};
	gdbstub_send_packet(buf, sizeof(buf));
}

// Receive a packet of data in gdbstub.pkt_buf, acknowledging it;
// a corrupted packet is negative-acknowledged and awaited again,
// hence this only ever returns valid packets, of their data length.
// The packet-start scan is where the engine thread idles, hence where
// ^C is detected and where queued stop events get reported (through
// gdbstub_getc_wait()); an emergency session running from a trap uses
// the blocking reads instead and ignores ^C (gdb only sends it while
// awaiting a stop-reply, which an emergency session already sent).
static unsigned gdbstub_recv_packet (void) {
	while (1) {
		// A new packet attempt re-snapshots the wire epoch: when an
		// emergency session interposes, it consumes the bytes of the
		// exchange in flight, and the reception must restart at the
		// packet-start scan instead of awaiting bytes forever.
		unsigned epoch = gdbstub.wire_epoch;
		int c;
		while ((c = gdbstub_getc_wait()) != '$') { // Scan for a packet start.
			if (c == 0x03 && !_trap_savedctx())
				gdbstub_ctrlc();
			// Discard acknowledgments (ie: '+', '-') and noise.
		}
		unsigned len = 0;
		bool overflow = false;
		while ((c = gdbstub_getc_poll(epoch)) != '#') { // Read data until the checksum.
			if (c == -1)
				break; // Emergency interposed; restart.
			if (c == '$') { // Unexpected packet restart.
				len = 0;
				overflow = false;
				continue;
			}
			if (len < sizeof(gdbstub.pkt_buf))
				gdbstub.pkt_buf[len++] = c;
			else
				overflow = true;
		}
		if (c == -1)
			continue;
		char hex[2];
		int h0 = gdbstub_getc_poll(epoch);
		int h1 = gdbstub_getc_poll(epoch);
		if (h0 == -1 || h1 == -1)
			continue; // Emergency interposed; restart.
		hex[0] = h0;
		hex[1] = h1;
		char expected;
		if (!overflow && gdbstub_dec_hex(hex, 2, &expected, 1) == 0 &&
			(unsigned char)expected == gdbstub_checksum(gdbstub.pkt_buf, len)) {
			gdbstub_putc('+');
			return len;
		}
		gdbstub_putc('-');
	}
}

// Return whether the packet in gdbstub.pkt_buf begins with the string given as argument.
static bool gdbstub_pkt_is (unsigned pkt_len, char *s) {
	unsigned i = 0;
	while (s[i]) {
		if (i >= pkt_len || gdbstub.pkt_buf[i] != s[i])
			return false;
		i += 1;
	}
	return true;
}

// ****************************************************************************
// Register marshaling
// ****************************************************************************

// The trap frame packs the 29 saved GPRs in the order
// x1-x2,x5-x31 (ie: gdb's register order with the x0,gp,tp
// gaps removed, as those are not saved); insure that here.
_Static_assert(offsetof(_savedctx_t, t0) == 2*sizeof(uintptr_t), "frame layout");
_Static_assert(offsetof(_savedctx_t, t6) == 28*sizeof(uintptr_t), "frame layout");
_Static_assert(offsetof(_savedctx_t, epc) == 31*sizeof(uintptr_t), "frame layout");

// Fix the registers which are not stored in the trap frame:
// x0 is the constant zero, while gp/tp are process-global
// constants read live (writing them is ignored).
// Used by the emergency session which binds the trapping context.
static void gdbstub_regs_fix (void) {
	gdbstub.regs[0] = 0;
	__asm__ __volatile__ ("mv %0, gp\n" : "=r"(gdbstub.regs[3]));
	__asm__ __volatile__ ("mv %0, tp\n" : "=r"(gdbstub.regs[4]));
}

// Load gdbstub.regs from the trap frame.
static void gdbstub_regs_load (_savedctx_t *f) {
	uintptr_t *fr = &f->ra;
	gdbstub.regs[1] = fr[0]; // ra.
	gdbstub.regs[2] = fr[1]; // sp.
	for (unsigned i = 2; i < 29; ++i)
		gdbstub.regs[3+i] = fr[i]; // t0 through t6.
	gdbstub.regs[32] = f->epc; // pc.
	gdbstub_regs_fix();
}

// Store gdbstub.regs back into the trap frame,
// from which mret restores the resumed context.
static void gdbstub_regs_store (_savedctx_t *f) {
	uintptr_t *fr = &f->ra;
	fr[0] = gdbstub.regs[1]; // ra.
	fr[1] = gdbstub.regs[2]; // sp.
	for (unsigned i = 2; i < 29; ++i)
		fr[i] = gdbstub.regs[3+i]; // t0 through t6.
	f->epc = gdbstub.regs[32]; // pc.
}

// ****************************************************************************
// Selection context classifier
// ****************************************************************************

// The registers of the selection are read from (and written through to)
// its saved context, whose layout depends on how the thread stopped;
// classification and every context dereference happen within a single
// _preempt_disable() critical section, after validating each pointer
// (a bogus dereference would either recursively trap, self-deadlocking
// the engine, or terminate the simulation on an unmapped access).

// Return whether an OS structure pointer is safe to dereference: like
// gdbstub_memok() but also accepting the boot carve-out right above
// __heap_end, where the initial thread _thread_t (hence its wait-queue
// and timer links, which other threads' links can point back to) lives.
static bool gdbstub_osptr_ok (uintptr_t p, uintptr_t len) {
	if (p & 3)
		return false;
	if (gdbstub_memok(p, len))
		return true;
	extern uintptr_t __heap_end;
	uintptr_t top = ((__heap_end + 1023) & ~(uintptr_t)1023);
	return (p >= __heap_end && p < top && len <= (top - p));
}

// Return whether the argument plausibly points to a _thread_t; the
// initial thread lives above __heap_end, hence possibly outside the
// gdb-accessible bounds, and gets accepted explicitly.
static bool gdbstub_thrd_valid (_thread_t *t) {
	if ((uintptr_t)t & 3)
		return false;
	if (t && t == gdbstub.mainthrd)
		return true;
	return gdbstub_memok((uintptr_t)t, sizeof(_thread_t));
}

// Classify the thread saved context; on GDBSTUB_CTX_FULL, *fp receives
// the full trap frame; on GDBSTUB_CTX_COOP/FRESH, *scp receives the
// cooperative frame. Must be called with preemption disabled, and the
// output pointers used within the same critical section, so that the
// thread cannot be switched in while its context is being accessed.
static unsigned gdbstub_ctx_class (
	_thread_t *t, _savedctx_t **scp, _savedctx_t **fp) {
	if (!gdbstub_thrd_valid(t))
		return GDBSTUB_CTX_INVALID;
	if (t == gdbstub.engine) // The engine cannot inspect itself.
		return GDBSTUB_CTX_LIVE;
	_savedctx_t *sc = *(_savedctx_t * volatile *)&t->savedctx;
	if (!sc)
		return GDBSTUB_CTX_DEAD;
	if (sc == (_savedctx_t *)-1)
		return GDBSTUB_CTX_LIVE; // Initial thread running, never yet switched-out.
	if (!*(volatile uintptr_t *)&t->ctxsaved)
		return GDBSTUB_CTX_LIVE; // On-CPU or mid-switch.
	if (((uintptr_t)sc & 3) || !gdbstub_memok((uintptr_t)sc, sizeof(_savedctx_t)))
		return GDBSTUB_CTX_INVALID;
	if ((intptr_t)*(volatile uintptr_t *)&t->cpu < 0) {
		*scp = sc;
		return GDBSTUB_CTX_FRESH;
	}
	uintptr_t scratch = sc->scratch;
	if (scratch) {
		if ((scratch & 3) || !gdbstub_memok(scratch, sizeof(_savedctx_t)))
			return GDBSTUB_CTX_INVALID;
		*fp = (_savedctx_t *)scratch;
		return GDBSTUB_CTX_FULL;
	}
	*scp = sc;
	return GDBSTUB_CTX_COOP;
}

#define GDBSTUB_AVAIL(N) (gdbstub.avail |= ((uint64_t)1 << (N)))

// Load the staging gdbstub.regs/avail from the selection saved context;
// registers which cannot be known are left unavailable (gdb displays
// them as such from the 'x' characters of the g/p replies).
// Returns the classification.
static unsigned gdbstub_ctx_load (_thread_t *t) {
	for (unsigned i = 0; i < 33; ++i)
		gdbstub.regs[i] = 0;
	gdbstub.avail = 0;
	_savedctx_t *sc = 0, *fp = 0;
	_preempt_disable();
	unsigned cls = gdbstub_ctx_class(t, &sc, &fp);
	if (cls == GDBSTUB_CTX_INVALID || cls == GDBSTUB_CTX_DEAD)
		goto done; // All registers unavailable.
	// x0/gp are process-global constants; the tp slot
	// reports the selection pointer itself.
	gdbstub.regs[0] = 0;
	__asm__ __volatile__ ("mv %0, gp\n" : "=r"(gdbstub.regs[3]));
	gdbstub.regs[4] = (uintptr_t)t;
	GDBSTUB_AVAIL(0); GDBSTUB_AVAIL(3); GDBSTUB_AVAIL(4);
	switch (cls) {
	case GDBSTUB_CTX_FULL: {
		uintptr_t *fr = &fp->ra;
		gdbstub.regs[1] = fr[0]; // ra.
		gdbstub.regs[2] = fr[1]; // sp.
		for (unsigned i = 2; i < 29; ++i)
			gdbstub.regs[3+i] = fr[i]; // t0 through t6.
		gdbstub.regs[32] = fp->epc; // pc.
		gdbstub.avail = (((uint64_t)1 << 33) - 1);
		break;
	}
	case GDBSTUB_CTX_COOP:
		// Only the callee-saved registers survive a cooperative
		// switch; the thread resumes at the return address with
		// its stack right above the switch frame.
		gdbstub.regs[1] = sc->ra; GDBSTUB_AVAIL(1);
		gdbstub.regs[2] = (uintptr_t)(sc + 1); GDBSTUB_AVAIL(2);
		gdbstub.regs[8] = sc->s0; GDBSTUB_AVAIL(8);
		gdbstub.regs[9] = sc->s1; GDBSTUB_AVAIL(9);
		{
			uintptr_t *s2 = &sc->s2;
			for (unsigned i = 0; i < 10; ++i) { // s2 through s11.
				gdbstub.regs[18+i] = s2[i];
				GDBSTUB_AVAIL(18+i);
			}
		}
		gdbstub.regs[32] = sc->ra; GDBSTUB_AVAIL(32); // pc.
		break;
	case GDBSTUB_CTX_FRESH:
		// A created thread starts at its entry (kept in s1) with
		// its argument (kept in s0) as a0, returning in _thread_exit.
		gdbstub.regs[1] = sc->ra; GDBSTUB_AVAIL(1);
		gdbstub.regs[2] = (uintptr_t)(sc + 1); GDBSTUB_AVAIL(2);
		gdbstub.regs[10] = sc->s0; GDBSTUB_AVAIL(10); // a0.
		gdbstub.regs[32] = sc->s1; GDBSTUB_AVAIL(32); // pc.
		break;
	// GDBSTUB_CTX_LIVE: only x0/gp/tp above.
	}
done:
	_preempt_enable();
	return cls;
}

// Write a single register through to the selection saved context.
// Writes to the constants x0/gp/tp are accepted and ignored (gdb
// restores every register after an inferior call). Returns false
// when the register cannot be written for the current context.
static bool gdbstub_ctx_store (_thread_t *t, unsigned regno, uintptr_t val) {
	bool ok = false;
	_savedctx_t *sc = 0, *fp = 0;
	_preempt_disable();
	unsigned cls = gdbstub_ctx_class(t, &sc, &fp);
	switch (cls) {
	case GDBSTUB_CTX_FULL: {
		if (regno == 0 || regno == 3 || regno == 4) {
			ok = true;
			break;
		}
		uintptr_t *fr = &fp->ra;
		if (regno == 1)
			fr[0] = val;
		else if (regno == 2)
			fr[1] = val;
		else if (regno >= 5 && regno <= 31)
			fr[regno-3] = val;
		else if (regno == 32)
			fp->epc = val;
		else
			break;
		ok = true;
		break;
	}
	case GDBSTUB_CTX_COOP:
		if (regno == 0 || regno == 3 || regno == 4) {
			ok = true;
			break;
		}
		if (regno == 1)
			sc->ra = val;
		else if (regno == 8)
			sc->s0 = val;
		else if (regno == 9)
			sc->s1 = val;
		else if (regno >= 18 && regno <= 27)
			(&sc->s2)[regno-18] = val;
		else
			break;
		ok = true;
		break;
	case GDBSTUB_CTX_FRESH:
		if (regno == 0 || regno == 3 || regno == 4) {
			ok = true;
			break;
		}
		if (regno == 1)
			sc->ra = val;
		else if (regno == 10)
			sc->s0 = val; // a0.
		else if (regno == 32)
			sc->s1 = val; // pc.
		else
			break;
		ok = true;
		break;
	// GDBSTUB_CTX_INVALID/DEAD/LIVE: nothing writable.
	}
	_preempt_enable();
	return ok;
}

// Write every writable register of gdbstub.regs through to the
// selection saved context (ie: the G packet). Returns false when
// nothing is writable for the current context.
static bool gdbstub_ctx_store_all (_thread_t *t) {
	bool ok = false;
	_savedctx_t *sc = 0, *fp = 0;
	_preempt_disable();
	unsigned cls = gdbstub_ctx_class(t, &sc, &fp);
	switch (cls) {
	case GDBSTUB_CTX_FULL: {
		uintptr_t *fr = &fp->ra;
		fr[0] = gdbstub.regs[1]; // ra.
		fr[1] = gdbstub.regs[2]; // sp.
		for (unsigned i = 2; i < 29; ++i)
			fr[i] = gdbstub.regs[3+i]; // t0 through t6.
		fp->epc = gdbstub.regs[32]; // pc.
		ok = true;
		break;
	}
	case GDBSTUB_CTX_COOP:
		sc->ra = gdbstub.regs[1];
		sc->s0 = gdbstub.regs[8];
		sc->s1 = gdbstub.regs[9];
		{
			uintptr_t *s2 = &sc->s2;
			for (unsigned i = 0; i < 10; ++i)
				s2[i] = gdbstub.regs[18+i]; // s2 through s11.
		}
		ok = true;
		break;
	case GDBSTUB_CTX_FRESH:
		sc->ra = gdbstub.regs[1];
		sc->s0 = gdbstub.regs[10]; // a0.
		sc->s1 = gdbstub.regs[32]; // pc.
		ok = true;
		break;
	// GDBSTUB_CTX_INVALID/DEAD/LIVE: nothing writable.
	}
	_preempt_enable();
	return ok;
}

// ****************************************************************************
// Parked threads bookkeeping
// ****************************************************************************

// __gdbstub_parked[] is protected by gdbstub.plock; a thread context
// acquirer must hold it within _preempt_disable() (a no-op in a trap,
// where preemption is off by construction) so that it cannot be
// switched-out while holding the lock, which would deadlock a trap
// handler spinning on it.

// Return the entry index of a thread, or -1 when not parked.
// gdbstub.plock must be held.
static int gdbstub_parked_find (_thread_t *t) {
	for (uintptr_t i = 0; i < __gdbstub_parked_cnt; ++i) {
		if (__gdbstub_parked[i].thrd == t)
			return i;
	}
	return -1;
}

// Append an entry; returns false when the list is full.
// gdbstub.plock must be held.
static bool gdbstub_parked_add (_thread_t *t, uintptr_t sig, unsigned flags) {
	if (__gdbstub_parked_cnt >= GDBSTUB_PARKED_MAX)
		return false;
	__gdbstub_parked[__gdbstub_parked_cnt].thrd = t;
	__gdbstub_parked[__gdbstub_parked_cnt].sig = sig;
	__gdbstub_parked[__gdbstub_parked_cnt].flags = flags;
	__gdbstub_parked_cnt += 1;
	return true;
}

// Remove an entry, keeping the list compact in arrival order.
// gdbstub.plock must be held.
static void gdbstub_parked_del (unsigned idx) {
	__gdbstub_parked_cnt -= 1;
	for (uintptr_t i = idx; i < __gdbstub_parked_cnt; ++i)
		__gdbstub_parked[i] = __gdbstub_parked[i+1];
	__gdbstub_parked[__gdbstub_parked_cnt] = (__gdbstub_parked_t){0, 0, 0};
}

// Remove a thread from the parked list and resume it; does nothing if
// the thread is not parked. To be called from thread or trap context.
static void gdbstub_unpark (_thread_t *t) {
	_preempt_disable();
	while (_xchg(&gdbstub.plock, 1));
	int idx = gdbstub_parked_find(t);
	if (idx >= 0)
		gdbstub_parked_del(idx);
	_xchg(&gdbstub.plock, 0);
	if (idx >= 0 && !_is_thread_terminated(t))
		_thread_schedoncpu(t, t->cpu, t->pin);
	_preempt_enable();
}

// Record a resume in the __gdbstub_last() history.
static void gdbstub_last_push (_thread_t *t) {
	if (gdbstub.last_resumed[0] == t)
		return;
	gdbstub.last_resumed[1] = gdbstub.last_resumed[0];
	gdbstub.last_resumed[0] = t;
}

// Resume every thread the stub parked; used by detach/kill. The
// debug-shell is skipped: resting parked IS its normal state, and
// resuming it would loop forever (it re-parks right away, ie: the
// engine and the shell would ping-pong).
static void gdbstub_unpark_all (void) {
	while (1) {
		_thread_t *t = 0;
		_preempt_disable();
		while (_xchg(&gdbstub.plock, 1));
		for (uintptr_t i = 0; i < __gdbstub_parked_cnt; ++i) {
			if (__gdbstub_parked[i].thrd != gdbstub.shell) {
				t = __gdbstub_parked[i].thrd;
				gdbstub_parked_del(i);
				break;
			}
		}
		_xchg(&gdbstub.plock, 0);
		if (t && !_is_thread_terminated(t))
			_thread_schedoncpu(t, t->cpu, t->pin);
		_preempt_enable();
		if (!t)
			break;
	}
}

// Park the selection if it is a stoppable thread, so that ^C (and the
// ? packet) freeze what the user is looking at; not-stoppable contexts
// (the shell resting parked, a fresh or terminated thread, a bogus
// pointer, the engine, an already parked thread) are left untouched and
// only reported. When the selection was already parked with a pending
// stop event, the event is consumed and its signal returned so that
// the reply reflects the real stop reason; otherwise returns 0.
static uintptr_t gdbstub_park_selection (void) {
	_thread_t *t = __gdbstub_tp;
	uintptr_t evtsig = 0;
	_preempt_disable();
	while (_xchg(&gdbstub.plock, 1));
	int idx = gdbstub_parked_find(t);
	if (idx >= 0) {
		if (__gdbstub_parked[idx].flags & GDBSTUB_PARKED_EVT) {
			evtsig = __gdbstub_parked[idx].sig;
			__gdbstub_parked[idx].flags &= ~GDBSTUB_PARKED_EVT;
		}
		goto done;
	}
	if (__gdbstub_parked_cnt >= GDBSTUB_PARKED_MAX)
		goto done; // No room to track a park; report-only.
	if (_ncpu() > 1)
		goto done; // Parking is single-CPU scoped; report-only.
	if (t == gdbstub.shell)
		goto done;
	// Only a thread whose saved context classifies as at-rest (full
	// trap frame or cooperative) is parked; everything else — the
	// engine, a fresh or terminated thread, a running context, and
	// especially a bogus pointer whose bytes do not form a plausible
	// _thread_t — is report-only, insuring gdb-supplied garbage can
	// never reach the OS primitives below.
	{
		_savedctx_t *sc = 0, *fp = 0;
		unsigned cls = gdbstub_ctx_class(t, &sc, &fp);
		if (cls != GDBSTUB_CTX_FULL && cls != GDBSTUB_CTX_COOP)
			goto done;
	}
	if (t->state != _THREAD_STOPPED && t->state != _THREAD_RUNNING)
		goto done;
	if (*(volatile uintptr_t *)&t->cpu >= _ncpu())
		goto done;
	// The wait-queue and timer links below get walked and written
	// through by _thread_stop()/_timer_disarm(); refuse pointers a
	// genuine thread cannot have (its links live in RAM), so that a
	// fake _thread_t which passed the classification above cannot
	// send the OS writing through arbitrary pointers.
	if (t->wq && !gdbstub_osptr_ok((uintptr_t)t->wq, sizeof(_waitq_t)))
		goto done;
	if (t->z.l.prev &&
		(!gdbstub_osptr_ok((uintptr_t)t->z.l.prev, sizeof(_dlist_t)) ||
		 !gdbstub_osptr_ok((uintptr_t)t->z.l.next, sizeof(_dlist_t)) ||
		 t->z.cpu != _cpuid()))
		goto done;
	// A sleeping thread must have its wake-up _timer disarmed first,
	// otherwise its expiry would silently un-park the thread; a _timer
	// can only be disarmed from the CPU that armed it, which holds here
	// as the stub is single-CPU scoped.
	if (t->z.l.prev)
		_timer_disarm(&t->z);
	_thread_stop(t);
	gdbstub_parked_add(t, GDBSTUB_SIGINT, 0);
done:
	_xchg(&gdbstub.plock, 0);
	_preempt_enable();
	return evtsig;
}

// ****************************************************************************
// Single-step engine
// ****************************************************************************

// There is no hardware single-step; a step instead plants temporary
// ebreak at the successor instructions of the pc, computed decoding
// the instruction to be stepped (4 bytes instructions only, as there
// is no compressed extension). gdbstub.step_owner is the stepped
// thread: its next trap of any kind ends the step, while another
// thread hitting a plant is parked SUPPRESSED (without an event) and
// released when the step ends; a step armed by an emergency session
// can be owner-less, in which case the very next trap ends it.

#define GDBSTUB_EBREAK 0x00100073

// Immediate extractors, sign-extended.
static intptr_t gdbstub_bimm (uint32_t insn) {
	return ((((intptr_t)(int32_t)insn >> 31) << 12) |
		(((insn >> 7) & 0x1) << 11) |
		(((insn >> 25) & 0x3f) << 5) |
		(((insn >> 8) & 0xf) << 1));
}
static intptr_t gdbstub_jimm (uint32_t insn) {
	return ((((intptr_t)(int32_t)insn >> 31) << 20) |
		(((insn >> 12) & 0xff) << 12) |
		(((insn >> 20) & 0x1) << 11) |
		(((insn >> 21) & 0x3ff) << 1));
}
static intptr_t gdbstub_iimm (uint32_t insn) {
	return ((intptr_t)(int32_t)insn >> 20);
}

// Plant a temporary ebreak at the address given as argument;
// out-of-bounds or misaligned addresses are silently skipped.
static void gdbstub_step_plant (uintptr_t addr) {
	if ((addr & 3) || !gdbstub_memok(addr, 4))
		return;
	for (unsigned i = 0; i < gdbstub.step_cnt; ++i) {
		if (gdbstub.step_addr[i] == addr)
			return;
	}
	if (gdbstub.step_cnt >= GDBSTUB_STEP_MAX)
		return;
	gdbstub.step_addr[gdbstub.step_cnt] = addr;
	gdbstub.step_orig[gdbstub.step_cnt] = *(volatile uint32_t *)addr;
	*(volatile uint32_t *)addr = GDBSTUB_EBREAK;
	gdbstub_memsync(addr, 4);
	gdbstub.step_cnt += 1;
}

// Return the plant index for an address, or -1.
static int gdbstub_step_find (uintptr_t addr) {
	for (unsigned i = 0; i < gdbstub.step_cnt; ++i) {
		if (gdbstub.step_addr[i] == addr)
			return i;
	}
	return -1;
}

// Restore the instructions saved by gdbstub_step_plant().
// _preempt_disable() insures no other thread executes between the
// code writes and the icache invalidation (a no-op within a trap,
// where nothing else runs).
static void gdbstub_step_disarm (void) {
	if (!gdbstub.step_cnt)
		return;
	_preempt_disable();
	for (unsigned i = 0; i < gdbstub.step_cnt; ++i) {
		*(volatile uint32_t *)gdbstub.step_addr[i] = gdbstub.step_orig[i];
		gdbstub_memsync(gdbstub.step_addr[i], 4);
	}
	gdbstub.step_cnt = 0;
	gdbstub_fencei();
	_preempt_enable();
}

// Plant temporary ebreak at the successor instructions of the pc,
// decoding from the staged gdbstub.regs.
static void gdbstub_step_arm (void) {
	_preempt_disable();
	uintptr_t pc = gdbstub.regs[32];
	if ((pc & 3) || !gdbstub_memok(pc, 4))
		goto done;
	uint32_t insn = *(volatile uint32_t *)pc;
	uint32_t opc = (insn & 0x7f);
	if (opc == 0x63) { // BRANCH.
		gdbstub_step_plant(pc + 4);
		// A conditional self-branch (offset 0) must not be planted
		// over: the plant would replace the branch itself and trap
		// before the condition is ever evaluated; the fall-through
		// plant alone then ends the step when the loop exits.
		intptr_t off = gdbstub_bimm(insn);
		if (off)
			gdbstub_step_plant(pc + off);
	} else if (opc == 0x6f) { // JAL.
		gdbstub_step_plant(pc + gdbstub_jimm(insn));
	} else if (opc == 0x67) { // JALR.
		gdbstub_step_plant(
			(gdbstub.regs[(insn >> 15) & 0x1f] + gdbstub_iimm(insn)) &
			~(uintptr_t)1);
	} else if (opc == 0x2f && ((insn >> 27) == 0x02)) { // lr.w .
		// An ebreak planted within an lr/sc sequence would kill the
		// reservation making the sc fail forever; instead the whole
		// sequence is stepped over as follow: plant after its sc.w,
		// and at the destination of any branch leaving the sequence.
		uintptr_t scaddr = 0;
		for (uintptr_t a = (pc + 4); a < (pc + 4*16); a += 4) {
			if (!gdbstub_memok(a, 4))
				break;
			if ((*(volatile uint32_t *)a & 0x7f) == 0x2f &&
				((*(volatile uint32_t *)a >> 27) == 0x03) /* sc.w */) {
				scaddr = a;
				break;
			}
		}
		if (scaddr) {
			// The fall-through plant is the mandatory one; plant it
			// first so that hitting GDBSTUB_STEP_MAX sheds branch
			// destinations instead.
			gdbstub_step_plant(scaddr + 4);
			for (uintptr_t a = (pc + 4); a < scaddr; a += 4) {
				uint32_t x = *(volatile uint32_t *)a;
				if ((x & 0x7f) == 0x63) { // BRANCH.
					uintptr_t dst = (a + gdbstub_bimm(x));
					if (dst < pc || dst > (scaddr + 4))
						gdbstub_step_plant(dst);
				}
			}
		} else // No sc.w found; degenerate to stepping the lr.w alone.
			gdbstub_step_plant(pc + 4);
	} else
		gdbstub_step_plant(pc + 4);
	gdbstub_fencei();
done:
	_preempt_enable();
}

// End the in-flight step: disarm every plant and release the threads
// that were suppressed while it was in flight. Runs on: the step owner
// next trap of any kind, ^C, the ? packet, D/k, and the dispose of the
// owner; a missed release path would leave threads parked forever.
static void gdbstub_step_lift (void) {
	gdbstub.step_owner = 0;
	gdbstub_step_disarm();
	while (1) {
		_thread_t *t = 0;
		_preempt_disable();
		while (_xchg(&gdbstub.plock, 1));
		for (uintptr_t i = 0; i < __gdbstub_parked_cnt; ++i) {
			if (__gdbstub_parked[i].flags & GDBSTUB_PARKED_SUPPRESSED) {
				t = __gdbstub_parked[i].thrd;
				gdbstub_parked_del(i);
				break;
			}
		}
		_xchg(&gdbstub.plock, 0);
		if (t && !_is_thread_terminated(t))
			_thread_schedoncpu(t, t->cpu, t->pin);
		_preempt_enable();
		if (!t)
			break;
	}
}

// ****************************************************************************
// Continue-and-next selection helpers
// ****************************************************************************

// Resume the thread recorded by __gdbstub_next(); the engine applies it
// when gdb writes __gdbstub_tp (ie: the M packet of the documented
// idiom below), with the next c/s/? packets as fallback points.
static void gdbstub_resume_pending_apply (void) {
	_preempt_disable();
	while (_xchg(&gdbstub.plock, 1));
	_thread_t *t = gdbstub.resume_pending;
	gdbstub.resume_pending = 0;
	int idx = ((t) ? gdbstub_parked_find(t) : -1);
	if (idx >= 0)
		gdbstub_parked_del(idx);
	_xchg(&gdbstub.plock, 0);
	if (idx >= 0 && !_is_thread_terminated(t)) {
		gdbstub_last_push(t);
		_thread_schedoncpu(t, t->cpu, t->pin);
	}
	_preempt_enable();
}

// Called from gdb, at its prompt, as an inferior call:
//	set var __gdbstub_tp = __gdbstub_next()
// records that the current selection is to be resumed, pops the OLDEST
// queued stop event and returns its thread (the debug-shell when none),
// which the assignment above then makes the inspected selection.
// It must not write __gdbstub_tp itself, nor resume anything: it runs
// as an inferior call hosted ON the selection saved context, which gdb
// still uses to read the returned value and restore the registers it
// clobbered; the engine resumes the recorded thread once gdb performs
// the assignment (ie: writes __gdbstub_tp), after those steps completed.
__attribute__((used)) _thread_t *__gdbstub_next (void) {
	// A resume still pending from a previous call whose
	// assignment never happened is applied first.
	gdbstub_resume_pending_apply();
	_thread_t *ret;
	_preempt_disable();
	while (_xchg(&gdbstub.plock, 1));
	gdbstub.resume_pending = __gdbstub_tp;
	ret = gdbstub.shell;
	for (uintptr_t i = 0; i < __gdbstub_parked_cnt; ++i) {
		if (__gdbstub_parked[i].flags & GDBSTUB_PARKED_EVT) {
			__gdbstub_parked[i].flags &= ~GDBSTUB_PARKED_EVT;
			ret = __gdbstub_parked[i].thrd;
			break;
		}
	}
	_xchg(&gdbstub.plock, 0);
	_preempt_enable();
	return ret;
}

// Called from gdb the same way as __gdbstub_next():
//	set var __gdbstub_tp = __gdbstub_last()
// returns the thread most recently resumed by c/s/__gdbstub_next(),
// which the drain idiom above deselects; this selects it back (a ^C
// then parks and inspects it). The resume which started this very
// inferior call (ie: of the selection hosting it) is skipped over.
// Returns the debug-shell when none.
__attribute__((used)) _thread_t *__gdbstub_last (void) {
	_thread_t *t = gdbstub.last_resumed[0];
	if (t == __gdbstub_tp)
		t = gdbstub.last_resumed[1];
	return (t ? t : gdbstub.shell);
}

// Called by _thread_dispose() (through its weak reference) before the
// memory holding the _thread_t is freed: purge every stub reference to
// the thread; the selection falls back to the debug-shell.
void _gdbstub_thread_disposed (_thread_t *thrd) {
	if (gdbstub.step_owner == thrd)
		gdbstub_step_lift();
	_preempt_disable();
	while (_xchg(&gdbstub.plock, 1));
	int idx = gdbstub_parked_find(thrd);
	if (idx >= 0) // Terminated; unlink without resuming.
		gdbstub_parked_del(idx);
	if (gdbstub.resume_pending == thrd)
		gdbstub.resume_pending = 0;
	if (gdbstub.last_resumed[0] == thrd) {
		gdbstub.last_resumed[0] = gdbstub.last_resumed[1];
		gdbstub.last_resumed[1] = 0;
	}
	if (gdbstub.last_resumed[1] == thrd)
		gdbstub.last_resumed[1] = 0;
	// Within the critical section, so that this check-then-write
	// cannot clobber a selection gdb is concurrently writing.
	if (__gdbstub_tp == thrd)
		__gdbstub_tp = gdbstub.shell;
	_xchg(&gdbstub.plock, 0);
	_preempt_enable();
}

// ****************************************************************************
// Stop events reporting
// ****************************************************************************

// Return whether a stop-reply is due: gdb awaits one (ie: after c/s)
// and the SELECTION has a queued stop event; the events of the other
// threads are never reported, they stay queued until drained with
// __gdbstub_next().
static bool gdbstub_event_due (void) {
	if (!gdbstub.resumed)
		return false;
	_thread_t *t = __gdbstub_tp;
	for (uintptr_t i = 0; i < __gdbstub_parked_cnt; ++i) {
		if ((__gdbstub_parked[i].flags & GDBSTUB_PARKED_EVT) &&
			__gdbstub_parked[i].thrd == t)
			return true;
	}
	return false;
}

// Send the due stop-reply, if any, consuming the event.
static void gdbstub_event_pump (void) {
	if (!gdbstub.resumed)
		return;
	_thread_t *t = __gdbstub_tp;
	uintptr_t sig = 0;
	_preempt_disable();
	while (_xchg(&gdbstub.plock, 1));
	for (uintptr_t i = 0; i < __gdbstub_parked_cnt; ++i) {
		if ((__gdbstub_parked[i].flags & GDBSTUB_PARKED_EVT) &&
			__gdbstub_parked[i].thrd == t) {
			sig = __gdbstub_parked[i].sig;
			__gdbstub_parked[i].flags &= ~GDBSTUB_PARKED_EVT;
			break;
		}
	}
	_xchg(&gdbstub.plock, 0);
	_preempt_enable();
	if (sig) {
		gdbstub.signum = sig;
		gdbstub.resumed = false;
		gdbstub_send_signal(sig);
	}
}

// Read a byte for a packet start; the SINGLE sleep site of the engine
// thread, where it idles between packets: report any due stop event,
// then sleep until woken by the serial interrupt or a trap queueing an
// event (with a timeout as insurance against a missed wake-up).
// The serial interrupt is level-triggered and one-shot (it disables
// itself when acknowledged): it must be re-armed before the final
// receive-buffer check, within the same IRQs-off section as the sleep,
// so that a byte landing in-between pends the interrupt across the
// sleep instead of being missed.
// Within a trap (ie: an emergency session), degrades to the blocking read.
static int gdbstub_getc_wait (void) {
	if (_trap_savedctx())
		return gdbstub_getc();
	while (1) {
		// Availability check and read within one preemption-off
		// section, for the same reason as gdbstub_getc_poll().
		_preempt_disable();
		if (gdbstub_rxusage()) {
			int c = gdbstub_getc();
			_preempt_enable();
			return c;
		}
		_preempt_enable();
		gdbstub_event_pump();
		gdbstub_setintr(1);
		_preempt_disable();
		if (!gdbstub_rxusage() && !gdbstub_event_due())
			_thread_sleeponwquntil(&gdbstub.wq, (_clkcycles() + _MSECS(50)));
		_preempt_enable();
	}
}

// ****************************************************************************
// ^C
// ****************************************************************************

// Raw 0x03 byte: gdb sends it only while awaiting a stop-reply. The
// selection is NEVER changed: the selection is parked if stoppable and
// SIGINT is reported (with the real stop reason instead when a stop
// event of the selection was already queued). An in-flight step is
// lifted first, so that no plant outlives the resume it belongs to.
static void gdbstub_ctrlc (void) {
	gdbstub_step_lift();
	uintptr_t sig = gdbstub_park_selection();
	if (!sig)
		sig = GDBSTUB_SIGINT;
	gdbstub.signum = sig;
	gdbstub.resumed = false;
	gdbstub_send_signal(sig);
}

// ****************************************************************************
// Engine session
// ****************************************************************************

// Reply the staged registers; unavailable ones read as 'x' characters,
// which gdb displays as such.
static void gdbstub_reply_regs (void) {
	unsigned pos = 0;
	for (unsigned i = 0; i < 33; ++i) {
		if (gdbstub.avail & ((uint64_t)1 << i)) {
			gdbstub_enc_hex((gdbstub.pkt_buf + pos),
				(sizeof(gdbstub.pkt_buf) - pos),
				(char *)&gdbstub.regs[i], sizeof(gdbstub.regs[i]));
		} else {
			for (unsigned k = 0; k < (2*sizeof(uintptr_t)); ++k)
				gdbstub.pkt_buf[pos+k] = 'x';
		}
		pos += (2*sizeof(uintptr_t));
	}
	gdbstub_send_packet(gdbstub.pkt_buf, pos);
}

// Service one gdb packet from gdbstub.pkt_buf; the engine thread loops
// on this. The c/s packets only flag the resumption (gdbstub.resumed):
// the stop-reply they expect is later sent by gdbstub_event_pump().
static void gdbstub_dispatch (unsigned pkt_len) {

	char *ptr = (gdbstub.pkt_buf + 1);
	char *end = (gdbstub.pkt_buf + pkt_len);
	uintptr_t addr, length;
	int status;

	switch (gdbstub.pkt_buf[0]) {

	// Read all registers.
	case 'g':
		gdbstub_ctx_load(__gdbstub_tp);
		gdbstub_reply_regs();
		break;

	// Write all registers.
	case 'G':
		status = gdbstub_dec_hex(ptr, (end - ptr),
			(char *)gdbstub.regs, sizeof(gdbstub.regs));
		if (status)
			goto error;
		if (!gdbstub_ctx_store_all(__gdbstub_tp))
			goto error;
		gdbstub_send_str("OK");
		break;

	// Read a register.
	case 'p':
		if (!gdbstub_parsehex(&ptr, end, &addr))
			goto error;
		if (addr >= 33)
			goto error;
		gdbstub_ctx_load(__gdbstub_tp);
		if (gdbstub.avail & ((uint64_t)1 << addr)) {
			status = gdbstub_enc_hex(
				gdbstub.pkt_buf, sizeof(gdbstub.pkt_buf),
				(char *)&gdbstub.regs[addr], sizeof(gdbstub.regs[addr]));
			gdbstub_send_packet(gdbstub.pkt_buf, status);
		} else {
			for (unsigned k = 0; k < (2*sizeof(uintptr_t)); ++k)
				gdbstub.pkt_buf[k] = 'x';
			gdbstub_send_packet(gdbstub.pkt_buf, (2*sizeof(uintptr_t)));
		}
		break;

	// Write a register; written through to the selection saved
	// context right away, as gdb assumes a write is effective
	// once acknowledged.
	case 'P':
		if (!gdbstub_parsehex(&ptr, end, &addr))
			goto error;
		if (ptr >= end || *ptr++ != '=')
			goto error;
		if (addr < 33) {
			uintptr_t val;
			status = gdbstub_dec_hex(ptr, (end - ptr),
				(char *)&val, sizeof(val));
			if (status)
				goto error;
			if (!gdbstub_ctx_store(__gdbstub_tp, addr, val))
				goto error;
		}
		gdbstub_send_str("OK");
		break;

	// Read memory.
	case 'm':
		if (!gdbstub_parsehex(&ptr, end, &addr))
			goto error;
		if (ptr >= end || *ptr++ != ',')
			goto error;
		if (!gdbstub_parsehex(&ptr, end, &length))
			goto error;
		if (length > sizeof(gdbstub.mem_buf))
			length = sizeof(gdbstub.mem_buf); // A truncated reply is valid.
		if (!gdbstub_memok(addr, length))
			goto error;
		for (uintptr_t i = 0; i < length; ++i)
			gdbstub.mem_buf[i] = *(volatile char *)(addr + i);
		status = gdbstub_enc_hex(
			gdbstub.pkt_buf, sizeof(gdbstub.pkt_buf),
			gdbstub.mem_buf, length);
		if (status == -1)
			goto error;
		gdbstub_send_packet(gdbstub.pkt_buf, status);
		break;

	// Write memory; used by gdb to plant its breakpoints,
	// hence the coherency dance for the modified words, with
	// preemption disabled so that no other thread executes
	// between the writes and the icache invalidation.
	// A write covering __gdbstub_tp is the assignment of the
	// __gdbstub_next() idiom: the recorded resume is applied.
	case 'M':
		if (!gdbstub_parsehex(&ptr, end, &addr))
			goto error;
		if (ptr >= end || *ptr++ != ',')
			goto error;
		if (!gdbstub_parsehex(&ptr, end, &length))
			goto error;
		if (ptr >= end || *ptr++ != ':')
			goto error;
		if (length > sizeof(gdbstub.mem_buf) || !gdbstub_memok(addr, length))
			goto error;
		status = gdbstub_dec_hex(ptr, (end - ptr),
			gdbstub.mem_buf, length);
		if (status)
			goto error;
		_preempt_disable();
		for (uintptr_t i = 0; i < length; ++i)
			*(volatile char *)(addr + i) = gdbstub.mem_buf[i];
		gdbstub_memsync(addr, length);
		gdbstub_fencei();
		_preempt_enable();
		if (addr <= (uintptr_t)&__gdbstub_tp &&
			(addr + length) >= ((uintptr_t)&__gdbstub_tp + sizeof(__gdbstub_tp)))
			gdbstub_resume_pending_apply();
		gdbstub_send_str("OK");
		break;

	// Report the last signal; gdb probes this when attaching.
	// Behaves as ^C except for the signal reported, so that
	// attaching to a running selection freezes it; with the
	// boot selection (the debug-shell) it perturbs nothing.
	case '?': {
		gdbstub_resume_pending_apply();
		gdbstub_step_lift();
		// A probing gdb is at its prompt: no stop-reply is awaited
		// anymore, even if a previous gdb was killed mid-resume
		// (a stale flag would later emit an unsolicited stop-reply).
		gdbstub.resumed = false;
		uintptr_t sig = gdbstub_park_selection();
		if (sig)
			gdbstub.signum = sig;
		gdbstub_send_signal(gdbstub.signum);
		break;
	}

	// Queries.
	case 'q':
		if (gdbstub_pkt_is(pkt_len, "qSupported")) {
			gdbstub_send_str("PacketSize=3f8");
		} else if (gdbstub_pkt_is(pkt_len, "qAttached")) {
			gdbstub_send_str("1");
		} else // Unsupported.
			gdbstub_send_packet(NULL, 0);
		break;

	// Continue; the C variant carries a signal number
	// which cannot be delivered, resuming is all that
	// can be done.
	case 'C':
		gdbstub_parsehex(&ptr, end, &addr); // Signal number, ignored.
		if (ptr < end && *ptr == ';')
			ptr += 1;
		// fallthrough.
	case 'c': {
		gdbstub_resume_pending_apply();
		_thread_t *t = __gdbstub_tp;
		if (gdbstub_parsehex(&ptr, end, &addr))
			gdbstub_ctx_store(t, 32, addr); // Only a full frame takes a pc.
		gdbstub.resumed = true;
		_preempt_disable();
		while (_xchg(&gdbstub.plock, 1));
		int idx = gdbstub_parked_find(t);
		bool suppressed = (idx >= 0 &&
			(__gdbstub_parked[idx].flags & GDBSTUB_PARKED_SUPPRESSED));
		_xchg(&gdbstub.plock, 0);
		_preempt_enable();
		// Resume the selection if the stub parked it; a SUPPRESSED
		// thread is left held: it would immediately re-enter the
		// in-flight step plant it sits on. A not-parked selection
		// (ie: running, or the OS blocked it) is left untouched:
		// its next stop event will be reported.
		if (idx >= 0 && !suppressed) {
			gdbstub_last_push(t);
			gdbstub_unpark(t);
		}
		break;
	}

	// Single-step; same signal handling as continue. Only a
	// selection parked with a full trap frame can be stepped;
	// anything else no-ops with an immediate S05 (an error
	// reply would abort the gdb resume machinery).
	case 'S':
		gdbstub_parsehex(&ptr, end, &addr); // Signal number, ignored.
		if (ptr < end && *ptr == ';')
			ptr += 1;
		// fallthrough.
	case 's': {
		gdbstub_resume_pending_apply();
		_thread_t *t = __gdbstub_tp;
		bool haveaddr = (gdbstub_parsehex(&ptr, end, &addr) != 0);
		_preempt_disable();
		while (_xchg(&gdbstub.plock, 1));
		int idx = gdbstub_parked_find(t);
		bool suppressed = (idx >= 0 &&
			(__gdbstub_parked[idx].flags & GDBSTUB_PARKED_SUPPRESSED));
		_xchg(&gdbstub.plock, 0);
		_preempt_enable();
		unsigned cls = gdbstub_ctx_load(t); // Also stages regs for the planter.
		// The debug-shell is never stepped: it rests ON its ebreak,
		// which would re-park it silently, never answering the step.
		if (idx < 0 || suppressed || cls != GDBSTUB_CTX_FULL ||
			gdbstub.step_owner || t == gdbstub.shell) {
			gdbstub.signum = GDBSTUB_SIGTRAP;
			gdbstub_send_signal(GDBSTUB_SIGTRAP);
			break;
		}
		if (haveaddr) {
			gdbstub.regs[32] = addr;
			gdbstub_ctx_store(t, 32, addr);
		}
		// The owner must be set BEFORE arming: with plants live and
		// no owner yet, any other thread trapping in-between would
		// end the step as owner-less, disarming the fresh plants.
		gdbstub.step_owner = t;
		gdbstub_step_arm();
		if (!gdbstub.step_cnt) // Nothing plantable; resumes free-running.
			gdbstub.step_owner = 0;
		gdbstub.resumed = true;
		gdbstub_last_push(t);
		gdbstub_unpark(t);
		break;
	}

	// Detach; gdb has already removed its breakpoints: end any
	// in-flight step and resume everything the stub parked.
	case 'D':
		gdbstub_step_lift();
		gdbstub.resume_pending = 0;
		gdbstub.resumed = false;
		gdbstub_unpark_all();
		gdbstub_send_str("OK");
		break;

	// Kill; there is nothing to kill, behave as a detach.
	case 'k':
		gdbstub_step_lift();
		gdbstub.resume_pending = 0;
		gdbstub.resumed = false;
		gdbstub_unpark_all();
		break;

	// Unsupported; the empty reply makes
	// gdb fall back to the packets above.
	default:
		gdbstub_send_packet(NULL, 0);
	}

	return;

error:
	gdbstub_send_str("E14");
}

// The engine thread: services gdb packets forever while the
// application runs; created by the constructor.
static void gdbstub_engine (void *arg) {
	(void)arg;
	while (1) {
		unsigned pkt_len = gdbstub_recv_packet();
		if (pkt_len == 0) {
			gdbstub_send_packet(NULL, 0);
			continue;
		}
		gdbstub_dispatch(pkt_len);
	}
}

// ****************************************************************************
// Emergency session
// ****************************************************************************

// Synchronous debugging session servicing gdb from within the trap,
// freezing the whole machine, exactly as the v1 stub did for every
// stop; used for the contexts which cannot park themselves: a nested
// trap (ie: _oops(), or a fault inside a trap handler), the idle
// context, the engine thread itself (ie: a breakpoint planted in stub
// internals), a fault taken before the constructor ran, and the
// parked list being full.
// It binds the trapping context (__gdbstub_tp is ignored), staging its
// registers in gdbstub.regs and writing them back when resuming.
static void gdbstub_emergency (uintptr_t signum) {

	gdbstub_bounds_init(); // Lazy; insures pre-constructor faults are serviceable.

	_savedctx_t *f = _trap_savedctx();
	gdbstub_wire_acquire((void *)f);

	// This session is about to consume every byte gdb sends: any
	// exchange the frozen engine had in flight is void; bumping the
	// wire epoch makes it abandon it and resync at the packet-start
	// scan once it runs again (see gdbstub_getc_poll()).
	gdbstub.wire_epoch += 1;

	// The trap may have frozen another context (typically the engine
	// polling gdbstub_rxusage()) mid device-command handshake; force
	// the device back to command-ready first, else the accessors
	// below would wait forever for the never-completed command.
	gdbstub_dev_recover();

	// Disable the serial device interrupt so that engine wake-ups do
	// not pile up while the session consumes bytes; re-enabled when
	// resuming.
	gdbstub_setintr(0);

	// Restore instructions planted by a single-step, so that gdb
	// observes the real memory content, and clear the step owner (a
	// stale owner would make every later step no-op, and its lift
	// guard could never fire again with the plants gone); the
	// suppressed threads, if any, stay held: the machine is frozen
	// anyway, and ^C/?/D/k release them once the world runs again.
	gdbstub_step_disarm();
	gdbstub.step_owner = 0;

	gdbstub_regs_load(f);
	gdbstub.signum = signum;

	if (gdbstub.resumed) {
		// gdb is awaiting the stop-reply of its last c/s packet;
		// when instead gdb is just attaching, nothing must be sent
		// unsolicited, it will probe using the ? packet.
		gdbstub.resumed = false;
		gdbstub_send_signal(signum);
	}

	while (1) {

		unsigned pkt_len = gdbstub_recv_packet();
		if (pkt_len == 0) {
			gdbstub_send_packet(NULL, 0);
			continue;
		}

		char *ptr = (gdbstub.pkt_buf + 1);
		char *end = (gdbstub.pkt_buf + pkt_len);
		uintptr_t addr, length;
		int status;

		switch (gdbstub.pkt_buf[0]) {

		// Read all registers.
		case 'g':
			status = gdbstub_enc_hex(
				gdbstub.pkt_buf, sizeof(gdbstub.pkt_buf),
				(char *)gdbstub.regs, sizeof(gdbstub.regs));
			gdbstub_send_packet(gdbstub.pkt_buf, status);
			break;

		// Write all registers.
		case 'G':
			status = gdbstub_dec_hex(ptr, (end - ptr),
				(char *)gdbstub.regs, sizeof(gdbstub.regs));
			if (status)
				goto error;
			gdbstub_regs_fix(); // Writes to x0/gp/tp are ignored.
			gdbstub_send_str("OK");
			break;

		// Read a register.
		case 'p':
			if (!gdbstub_parsehex(&ptr, end, &addr))
				goto error;
			if (addr >= 33)
				goto error;
			status = gdbstub_enc_hex(
				gdbstub.pkt_buf, sizeof(gdbstub.pkt_buf),
				(char *)&gdbstub.regs[addr], sizeof(gdbstub.regs[addr]));
			gdbstub_send_packet(gdbstub.pkt_buf, status);
			break;

		// Write a register.
		case 'P':
			if (!gdbstub_parsehex(&ptr, end, &addr))
				goto error;
			if (ptr >= end || *ptr++ != '=')
				goto error;
			if (addr < 33) {
				status = gdbstub_dec_hex(ptr, (end - ptr),
					(char *)&gdbstub.regs[addr],
					sizeof(gdbstub.regs[addr]));
				if (status)
					goto error;
				gdbstub_regs_fix(); // Writes to x0/gp/tp are ignored.
			}
			gdbstub_send_str("OK");
			break;

		// Read memory.
		case 'm':
			if (!gdbstub_parsehex(&ptr, end, &addr))
				goto error;
			if (ptr >= end || *ptr++ != ',')
				goto error;
			if (!gdbstub_parsehex(&ptr, end, &length))
				goto error;
			if (length > sizeof(gdbstub.mem_buf))
				length = sizeof(gdbstub.mem_buf); // A truncated reply is valid.
			if (!gdbstub_memok(addr, length))
				goto error;
			for (uintptr_t i = 0; i < length; ++i)
				gdbstub.mem_buf[i] = *(volatile char *)(addr + i);
			status = gdbstub_enc_hex(
				gdbstub.pkt_buf, sizeof(gdbstub.pkt_buf),
				gdbstub.mem_buf, length);
			if (status == -1)
				goto error;
			gdbstub_send_packet(gdbstub.pkt_buf, status);
			break;

		// Write memory; used by gdb to plant its breakpoints,
		// hence the coherency dance for the modified words.
		case 'M':
			if (!gdbstub_parsehex(&ptr, end, &addr))
				goto error;
			if (ptr >= end || *ptr++ != ',')
				goto error;
			if (!gdbstub_parsehex(&ptr, end, &length))
				goto error;
			if (ptr >= end || *ptr++ != ':')
				goto error;
			if (length > sizeof(gdbstub.mem_buf) || !gdbstub_memok(addr, length))
				goto error;
			status = gdbstub_dec_hex(ptr, (end - ptr),
				gdbstub.mem_buf, length);
			if (status)
				goto error;
			for (uintptr_t i = 0; i < length; ++i)
				*(volatile char *)(addr + i) = gdbstub.mem_buf[i];
			gdbstub_memsync(addr, length);
			gdbstub_fencei();
			gdbstub_send_str("OK");
			break;

		// Report the last signal.
		case '?':
			gdbstub_send_signal(gdbstub.signum);
			break;

		// Queries.
		case 'q':
			if (gdbstub_pkt_is(pkt_len, "qSupported")) {
				gdbstub_send_str("PacketSize=3f8");
			} else if (gdbstub_pkt_is(pkt_len, "qAttached")) {
				gdbstub_send_str("1");
			} else // Unsupported.
				gdbstub_send_packet(NULL, 0);
			break;

		// Continue; the C variant carries a signal number
		// which cannot be delivered, resuming is all that
		// can be done.
		case 'C':
			gdbstub_parsehex(&ptr, end, &addr); // Signal number, ignored.
			if (ptr < end && *ptr == ';')
				ptr += 1;
			// fallthrough.
		case 'c':
			if (gdbstub_parsehex(&ptr, end, &addr))
				gdbstub.regs[32] = addr;
			gdbstub.resumed = true;
			goto resume;

		// Single-step; same signal handling as continue.
		case 'S':
			gdbstub_parsehex(&ptr, end, &addr); // Signal number, ignored.
			if (ptr < end && *ptr == ';')
				ptr += 1;
			// fallthrough.
		case 's':
			if (gdbstub_parsehex(&ptr, end, &addr))
				gdbstub.regs[32] = addr;
			gdbstub_step_arm();
			// The trapping thread owns the step, so that its next
			// trap lifts the plants; when there is no thread (ie:
			// the idle context), the very next trap of any kind
			// lifts them (see gdbstub_trap()).
			gdbstub.step_owner = _thread_cur;
			gdbstub.resumed = true;
			goto resume;

		// Detach; gdb has already removed its breakpoints.
		case 'D':
			gdbstub_send_str("OK");
			goto resume;

		// Kill; there is nothing to kill, just resume.
		case 'k':
			goto resume;

		// Unsupported; the empty reply makes
		// gdb fall back to the packets above.
		default:
			gdbstub_send_packet(NULL, 0);
		}

		continue;

	error:
		gdbstub_send_str("E14");
	}

resume:
	gdbstub_regs_store(f);
	// Re-arm the serial device interrupt which
	// disables itself when acknowledged.
	gdbstub_setintr(1);
	gdbstub_wire_release((void *)f);
}

// ****************************************************************************
// Entry points
// ****************************************************************************

// The debug-shell resting instruction; a thread trapping there is the
// shell coming to rest, which parks silently (no stop event).
extern const char __gdbstub_shell_rest[];

// Common handling for every exception: decide between an emergency
// session and parking the trapping thread. Always returns false so
// that mepc is left unchanged: gdb removes/restores its ebreak before
// resuming, hence the original instruction re-executes (a faulting
// instruction re-executes as well, unless gdb changes the pc).
static bool gdbstub_trap (uintptr_t sig) {
	_savedctx_t *f = _trap_savedctx();
	// Contexts which cannot park themselves get the emergency session:
	// a nested trap (f->scratch is then non-null, which _oops() always
	// is, its ebreak executing within the ecall handling), the idle
	// context, the engine thread, a trap before the constructor, and
	// any context interrupted with IRQs disabled (mstatus.MPIE clear,
	// the same gate crt0.S tail-chaining uses): such a context can be
	// holding a spinlock — the stub's own parked-list lock, or an OS
	// run-queue lock — which the park paths below spin on, hence
	// parking it could deadlock the whole machine. This screen must
	// come before ANY lock acquisition of this function.
	// Per-thread parking is also single-CPU scoped (code writes lack
	// a cross-CPU icache invalidation, and cross-CPU stops, timer
	// disarms and context snapshots are unimplemented): with more
	// than one CPU running, every stop gets the frozen session too.
	if (f->scratch || !_thread_cur || _thread_cur == gdbstub.engine ||
		!gdbstub.engine || !(f->status & 0x80) || _ncpu() > 1) {
		gdbstub_emergency(sig);
		return false;
	}
	// Step bookkeeping, so that plants never outlive their step: any
	// trap of the step owner ends the step; an owner-less step (armed
	// by an emergency session for the idle context) is ended by the
	// very next trap of any kind; the emergency path above does its
	// own step cleanup.
	if (gdbstub.step_cnt &&
		(!gdbstub.step_owner || gdbstub.step_owner == _thread_cur))
		gdbstub_step_lift();
	// The debug-shell coming to rest parks silently: no stop event.
	if (_thread_cur == gdbstub.shell &&
		f->epc == (uintptr_t)__gdbstub_shell_rest) {
		while (_xchg(&gdbstub.plock, 1));
		bool ok = gdbstub_parked_add(_thread_cur, sig, 0);
		_xchg(&gdbstub.plock, 0);
		if (!ok) {
			gdbstub_emergency(sig);
			return false;
		}
		_thread_sleepuntil(_DATE_MAX); // Park-self; resumes through __trap_ret.
		return false; // Not reached.
	}
	// Another thread hitting an in-flight step plant is held SUPPRESSED
	// (without an event) until the step ends, unless the plant sits on
	// a gdb breakpoint (ie: the saved original instruction is itself an
	// ebreak), which is a true stop event.
	if (gdbstub.step_cnt && sig == GDBSTUB_SIGTRAP) {
		int i = gdbstub_step_find(f->epc);
		if (i >= 0 && gdbstub.step_orig[i] != GDBSTUB_EBREAK) {
			while (_xchg(&gdbstub.plock, 1));
			bool ok = gdbstub_parked_add(_thread_cur, sig,
				GDBSTUB_PARKED_SUPPRESSED);
			_xchg(&gdbstub.plock, 0);
			if (!ok) {
				gdbstub_emergency(sig);
				return false;
			}
			_thread_sleepuntil(_DATE_MAX); // Park-self.
			return false; // Not reached.
		}
	}
	// Normal stop event: queue it, wake the engine, park.
	while (_xchg(&gdbstub.plock, 1));
	bool ok = gdbstub_parked_add(_thread_cur, sig, GDBSTUB_PARKED_EVT);
	_xchg(&gdbstub.plock, 0);
	if (!ok) {
		gdbstub_emergency(sig);
		return false;
	}
	// Wake the engine BEFORE parking, so that the run-queue cannot
	// empty from within this trap handling.
	_thread_schedone(&gdbstub.wq);
	_thread_sleepuntil(_DATE_MAX); // Park-self; resumes through __trap_ret.
	return false; // Not reached.
}

// Breakpoint (gdb planted ebreak, or a linked-in ebreak such as _oops),
// or single-step completion; overrides the weak handler from _os.c.
bool __trap_exc_break (void) {
	return gdbstub_trap(GDBSTUB_SIGTRAP);
}

// All the remaining exceptions; a crashing application drops
// into the debugger instead of hanging in the weak handlers
// from _os.c; resuming re-executes the faulting instruction.
bool __trap_exc_insn_misaligned (void) {
	return gdbstub_trap(GDBSTUB_SIGBUS);
}
bool __trap_exc_insn_afault (void) {
	return gdbstub_trap(GDBSTUB_SIGSEGV);
}
bool __trap_exc_insn_illegal (void) {
	return gdbstub_trap(GDBSTUB_SIGILL);
}
bool __trap_exc_load_misaligned (void) {
	return gdbstub_trap(GDBSTUB_SIGBUS);
}
bool __trap_exc_store_misaligned (void) {
	return gdbstub_trap(GDBSTUB_SIGBUS);
}
bool __trap_exc_load_afault (void) {
	return gdbstub_trap(GDBSTUB_SIGSEGV);
}
bool __trap_exc_store_afault (void) {
	return gdbstub_trap(GDBSTUB_SIGSEGV);
}
bool __trap_exc_ecall_u (void) {
	return gdbstub_trap(GDBSTUB_SIGTRAP);
}
bool __trap_exc_ecall_s (void) {
	return gdbstub_trap(GDBSTUB_SIGTRAP);
}
bool __trap_exc_insn_pfault (void) {
	return gdbstub_trap(GDBSTUB_SIGSEGV);
}
bool __trap_exc_load_pfault (void) {
	return gdbstub_trap(GDBSTUB_SIGSEGV);
}
bool __trap_exc_store_pfault (void) {
	return gdbstub_trap(GDBSTUB_SIGSEGV);
}
bool __trap_exc_inv (void) {
	return gdbstub_trap(GDBSTUB_SIGTRAP);
}

// Serial device interrupt; wake-only: the engine consumes the bytes in
// thread context and re-arms the interrupt itself before sleeping (see
// gdbstub_getc_wait()); re-arming here would storm this handler while
// bytes remain buffered, the interrupt being level-triggered.
static void gdbstub_irq (_irq_t *i) {
	(void)i;
	_thread_schedone(&gdbstub.wq);
}

// The debug-shell thread: rests on an ebreak which parks it silently
// with a full trap frame; a stable context to select which perturbs
// nothing when inspected, and the host for gdb inferior calls (which
// return to a gdb breakpoint, ie: away from the resting instruction,
// parking with a true stop event that reports the call completion).
static void gdbstub_shell (void *arg) {
	(void)arg;
	__asm__ __volatile__ (
		"0:\n"
		".global __gdbstub_shell_rest\n"
		"__gdbstub_shell_rest:\n"
		"ebreak\n"
		"j 0b\n"
		::: "memory");
}

static _irq_t gdbstub_irq_st;

// Static stacks for the stub threads; _thread_create() carves the TLS
// and _thread_t areas from their top. The shell stack also hosts gdb
// inferior calls, which should stay modest.
static uint8_t gdbstub_engine_stack[2048] __attribute__((aligned(16)));
static uint8_t gdbstub_shell_stack[1024] __attribute__((aligned(16)));

__attribute__((constructor)) static void gdbstub_init (void) {
	gdbstub_bounds_init();
	// The constructor runs on the initial thread, whose _thread_t
	// (living above __heap_end, hence possibly outside the memory
	// bounds) must be recognized by the thread-pointer validator.
	gdbstub.mainthrd = _thread_cur;
	gdbstub.shell = _thread_create(
		gdbstub_shell_stack, sizeof(gdbstub_shell_stack), gdbstub_shell, 0);
	gdbstub.engine = _thread_create(
		gdbstub_engine_stack, sizeof(gdbstub_engine_stack), gdbstub_engine, 0);
	// The boot selection is the debug-shell; set before any of the two
	// threads can run, as gdbstub_trap() dispatches on those fields.
	__gdbstub_tp = gdbstub.shell;
	_thread_sched(gdbstub.engine);
	_thread_sched(gdbstub.shell);
	// Insure the selection functions survive the linker section
	// garbage-collection: nothing else references them, they exist
	// to be called from gdb.
	__asm__ __volatile__ ("" ::
		"r"(__gdbstub_next), "r"(__gdbstub_last), "r"(__gdbstub_parked));
	_irq_init(&gdbstub_irq_st, _gdbstub_irq, gdbstub_irq);
	_irq_register(&gdbstub_irq_st);
	_preempt_disable(); // Insure the irqctrl transaction is not interrupted.
	hwdrvirqctrl_ena(_gdbstub_irq, 1);
	_preempt_enable();
	gdbstub_setintr(1);
}
