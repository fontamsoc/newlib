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

// Single static instance; buffers must not live on the
// stack as traps run on the small per-CPU __trap_stacks.
static struct {
	uintptr_t signum;      // Last stop signal; what the ? packet reports.
	uintptr_t regs[33];    // Per gdb register order: x0-x31, pc.
	bool resumed;          // Set when gdb is awaiting a stop-reply (ie: after c/s).
	int swallowed;         // Byte pre-consumed by the _irq handler; -1 when none.
	uintptr_t membeg;      // Memory bounds gdb is allowed to access.
	uintptr_t memend;
	uintptr_t lock;        // Serializes debugging sessions between CPUs.
	unsigned step_cnt;     // Temporary breakpoints planted by a single-step.
	uintptr_t step_addr[GDBSTUB_STEP_MAX];
	uint32_t step_orig[GDBSTUB_STEP_MAX];
	char pkt_buf[1024];    // Packet buffer; qSupported reports (sizeof-8) as PacketSize.
	char mem_buf[512];     // m/M packets data staging buffer.
} gdbstub = {.signum = GDBSTUB_SIGTRAP, .swallowed = -1};

// ****************************************************************************
// Serial device accessors
// ****************************************************************************

// hwdrvchar_t instance used with <machine/hwdrvchar.h>; its field addr
// gets loaded from the weak _gdbstub_dev at each use; hwdrvchar_init()
// is not needed as only hwdrvchar_readable() and hwdrvchar_interrupt()
// are used, which do not depend on the fields it fills.
static hwdrvchar_t gdbstub_hwdrvchar;

// Read a byte from the serial device; it blocks
// the bus until a byte is available, hence there
// is no failure case and no need to poll.
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
// retransmits until gdb acknowledges it.
static void gdbstub_send_packet (char *pkt_data, unsigned pkt_len) {
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
			c = gdbstub_getc();
		} while (c != '+' && c != '-');
		if (c == '+')
			return;
	}
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
static unsigned gdbstub_recv_packet (void) {
	while (1) {
		if (gdbstub.swallowed == '$') // Packet start byte consumed by the _irq handler.
			gdbstub.swallowed = -1;
		else while (gdbstub_getc() != '$');
		unsigned len = 0;
		bool overflow = false;
		int c;
		while ((c = gdbstub_getc()) != '#') { // Read data until the checksum.
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
		char hex[2];
		hex[0] = gdbstub_getc();
		hex[1] = gdbstub_getc();
		char expected;
		if (!overflow && gdbstub_dec_hex(hex, 2, &expected, 1) == 0 &&
			(unsigned char)expected == gdbstub_checksum(gdbstub.pkt_buf, len)) {
			gdbstub_putc('+');
			return len;
		}
		gdbstub_putc('-');
	}
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
// Single-step engine
// ****************************************************************************

// There is no hardware single-step; a step instead plants temporary
// ebreak at the successor instructions of the pc, computed decoding
// the instruction to be stepped (4 bytes instructions only, as there
// is no compressed extension).

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

// Restore the instructions saved by gdbstub_step_plant().
static void gdbstub_step_disarm (void) {
	if (!gdbstub.step_cnt)
		return;
	for (unsigned i = 0; i < gdbstub.step_cnt; ++i) {
		*(volatile uint32_t *)gdbstub.step_addr[i] = gdbstub.step_orig[i];
		gdbstub_memsync(gdbstub.step_addr[i], 4);
	}
	gdbstub.step_cnt = 0;
	gdbstub_fencei();
}

// Plant temporary ebreak at the successor instructions of the pc.
static void gdbstub_step_arm (void) {
	uintptr_t pc = gdbstub.regs[32];
	if ((pc & 3) || !gdbstub_memok(pc, 4))
		return;
	uint32_t insn = *(volatile uint32_t *)pc;
	uint32_t opc = (insn & 0x7f);
	if (opc == 0x63) { // BRANCH.
		gdbstub_step_plant(pc + 4);
		gdbstub_step_plant(pc + gdbstub_bimm(insn));
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
}

// ****************************************************************************
// Session
// ****************************************************************************

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

// Debugging session; services gdb packets until resumed.
// The argument is the signal number to report to gdb.
static void gdbstub_session (uintptr_t signum) {

	while (_xchg(&gdbstub.lock, 1)); // Insure a single session between CPUs.

	// Disable the serial device interrupt so that another CPU does not
	// steal the session bytes; re-enabled when resuming.
	gdbstub_setintr(0);

	// Restore instructions planted by a single-step, so that gdb
	// observes the real memory content; under the lock, so that
	// another CPU cannot tear the plant bookkeeping.
	gdbstub_step_disarm();

	_savedctx_t *f = _trap_savedctx();
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
	_xchg(&gdbstub.lock, 0);
}

// ****************************************************************************
// Entry points
// ****************************************************************************

// Breakpoint (gdb planted ebreak, or a linked-in ebreak such as _oops),
// or single-step completion; overrides the weak handler from _os.c.
// Returns false so that mepc is left unchanged: gdb removes/restores its
// ebreak before resuming, hence the original instruction re-executes.
bool __trap_exc_break (void) {
	gdbstub_session(GDBSTUB_SIGTRAP);
	return false;
}

// All the remaining exceptions; a crashing application drops
// into the debugger instead of hanging in the weak handlers
// from _os.c; resuming re-executes the faulting instruction.
bool __trap_exc_insn_misaligned (void) {
	gdbstub_session(GDBSTUB_SIGBUS);
	return false;
}
bool __trap_exc_insn_afault (void) {
	gdbstub_session(GDBSTUB_SIGSEGV);
	return false;
}
bool __trap_exc_insn_illegal (void) {
	gdbstub_session(GDBSTUB_SIGILL);
	return false;
}
bool __trap_exc_load_misaligned (void) {
	gdbstub_session(GDBSTUB_SIGBUS);
	return false;
}
bool __trap_exc_store_misaligned (void) {
	gdbstub_session(GDBSTUB_SIGBUS);
	return false;
}
bool __trap_exc_load_afault (void) {
	gdbstub_session(GDBSTUB_SIGSEGV);
	return false;
}
bool __trap_exc_store_afault (void) {
	gdbstub_session(GDBSTUB_SIGSEGV);
	return false;
}
bool __trap_exc_ecall_u (void) {
	gdbstub_session(GDBSTUB_SIGTRAP);
	return false;
}
bool __trap_exc_ecall_s (void) {
	gdbstub_session(GDBSTUB_SIGTRAP);
	return false;
}
bool __trap_exc_insn_pfault (void) {
	gdbstub_session(GDBSTUB_SIGSEGV);
	return false;
}
bool __trap_exc_load_pfault (void) {
	gdbstub_session(GDBSTUB_SIGSEGV);
	return false;
}
bool __trap_exc_store_pfault (void) {
	gdbstub_session(GDBSTUB_SIGSEGV);
	return false;
}
bool __trap_exc_inv (void) {
	gdbstub_session(GDBSTUB_SIGTRAP);
	return false;
}

// Serial device interrupt; fires on the first byte received while
// the application is running: either gdb attaching (ie: a packet)
// or interrupting the application (ie: ^C).
static void gdbstub_irq (_irq_t *i) {
	while (gdbstub_rxusage()) {
		int c = gdbstub_getc();
		if (c == 0x03) { // ^C; gdb is awaiting a stop-reply for it.
			gdbstub.resumed = true;
			gdbstub_session(GDBSTUB_SIGINT);
			break;
		}
		if (c == '$') { // gdb attaching or transmitting a packet.
			gdbstub.swallowed = c;
			gdbstub_session(GDBSTUB_SIGTRAP);
			break;
		}
		// Discard acknowledgments (ie: '+', '-') and noise.
	}
	gdbstub_setintr(1);
}

static _irq_t gdbstub_irq_st;

__attribute__((constructor)) static void gdbstub_init (void) {
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
	_irq_init(&gdbstub_irq_st, _gdbstub_irq, gdbstub_irq);
	_irq_register(&gdbstub_irq_st);
	_preempt_disable(); // Insure the irqctrl transaction is not interrupted.
	hwdrvirqctrl_ena(_gdbstub_irq, 1);
	_preempt_enable();
	gdbstub_setintr(1);
}
