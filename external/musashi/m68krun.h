/* PICO-AMIGA: the loop that runs a handler set, and its memory accessors.
 *
 * Included at the end of gen/m68kops.c (tools/gen_musashi.py), so there is one
 * copy per CPU type, next to its handlers (the same SRAM window on the board)
 * and in their view of the CPU state (m68kregs.h, m68kcpu.h): the PC, the
 * cycles and the instruction word live in registers from M68KI_LOAD() to
 * M68KI_SAVE().  m68k_execute() (m68kcpu.c) calls it through m68ki_run /
 * m68ki_run_traced. */
#ifndef M68KRUN__HEADER
#define M68KRUN__HEADER

#include <string.h>

/* The opcode -> handler index (flash) and the running set's handlers and
 * cycles (m68kcpu.c's copies in SRAM). */
extern const unsigned short m68ki_handler_index[0x10000];
extern void (*m68ki_handlers[])(void);
extern unsigned char m68ki_handler_cycles[];

#ifdef AMIGA_FETCH_WINDOW
/* A data access the handlers make themselves: counted for the chip bus model
 * when it is chip RAM (the banks below M68K_FAST_CHIP_TOP, mem.c). */
#ifdef AMIGA_STATS
#define M68KF_STAT() (amiga_stat_data++)
#else
#define M68KF_STAT() ((void)0)
#endif
#define M68KF_COUNT(a) do { M68KF_STAT(); if((a) < M68K_FAST_CHIP_TOP) m68ki_cpu.bus_data++; } while (0)
#endif

/* Through the callbacks: the registers stored before, loaded after.  Apart, so
 * that the accessors below are leaves that end in a jump here. */
#define M68KF_SLOW M68KI_HOT2 __attribute__((noinline, noclone, used))
M68KF_SLOW static uint m68kf_r8_slow(uint a)
{
	uint v;
	M68KI_SAVE();
	v = m68k_read_memory_8(a);
	M68KI_LOAD();
	return v;
}
M68KF_SLOW static uint m68kf_r16_slow(uint a)
{
	uint v;
	M68KI_SAVE();
	v = m68k_read_memory_16(a);
	M68KI_LOAD();
	return v;
}
M68KF_SLOW static uint m68kf_r32_slow(uint a)
{
	uint v;
	M68KI_SAVE();
	v = m68k_read_memory_32(a);
	M68KI_LOAD();
	return v;
}
M68KF_SLOW static void m68kf_w8_slow(uint a, uint v)
{
	M68KI_SAVE();
	m68k_write_memory_8(a, v);
	M68KI_LOAD();
}
M68KF_SLOW static void m68kf_w16_slow(uint a, uint v)
{
	M68KI_SAVE();
	m68k_write_memory_16(a, v);
	M68KI_LOAD();
}
M68KF_SLOW static void m68kf_w32_slow(uint a, uint v)
{
	M68KI_SAVE();
	m68k_write_memory_32(a, v);
	M68KI_LOAD();
}
/* An instruction word outside the fetch window. */
M68KF_SLOW static uint m68kf_fetch(uint a)
{
	uint v;
	M68KI_SAVE();
#ifdef AMIGA_FETCH_WINDOW
	v = m68ki_fetch_slow16(a);
#else
	v = m68k_read_memory_16(a);
#endif
	M68KI_LOAD();
	return v;
}

M68KI_HOT2 static __attribute__((noinline)) uint m68kf_r8(uint a)
{
#ifdef AMIGA_FETCH_WINDOW
	const unsigned char *const p = m68ki_cpu.fast[a >> 16].rd;
	if(p)
	{
		M68KF_COUNT(a);
		return p[a];
	}
#endif
	return m68kf_r8_slow(a);
}

M68KI_HOT2 static __attribute__((noinline)) uint m68kf_r16(uint a)
{
#ifdef AMIGA_FETCH_WINDOW
	const unsigned char *const p = m68ki_cpu.fast[a >> 16].rd;
	if(p)
	{
		M68KF_COUNT(a);
		return m68ki_fetch_be16(p + a);
	}
#endif
	return m68kf_r16_slow(a);
}

M68KI_HOT2 static __attribute__((noinline)) uint m68kf_r32(uint a)
{
#if defined(AMIGA_FETCH_WINDOW) && !defined(AMIGA_TRACE_VECTORS)
	const unsigned char *const p = m68ki_cpu.fast[a >> 16].rd;
	if(p && ((a + 2) & 0xffff) >= 2)             /* both halves in this bank */
	{
		unsigned int l;
		M68KF_COUNT(a);
		memcpy(&l, p + a, 4);
		return __builtin_bswap32(l);
	}
#endif
	return m68kf_r32_slow(a);
}

M68KI_HOT2 static __attribute__((noinline)) void m68kf_w8(uint a, uint v)
{
#ifdef AMIGA_FETCH_WINDOW
	unsigned char *const p = m68ki_cpu.fast[a >> 16].wr;
	if(p)
	{
		M68KF_STAT();
		p[a] = (unsigned char)v;
		if(a < M68K_FAST_CHIP_TOP)
		{
			m68ki_cpu.bus_data++;
			if(m68ki_cpu.fast_wmap[a >> 12])
				m68k_fast_mirror(a, v, 1);
		}
		return;
	}
#endif
	m68kf_w8_slow(a, v);
}

M68KI_HOT2 static __attribute__((noinline)) void m68kf_w16(uint a, uint v)
{
#ifdef AMIGA_FETCH_WINDOW
	unsigned char *const p = m68ki_cpu.fast[a >> 16].wr;
	if(p)
	{
		const unsigned short w = __builtin_bswap16((unsigned short)v);
		M68KF_STAT();
		memcpy(p + a, &w, 2);
		if(a < M68K_FAST_CHIP_TOP)
		{
			m68ki_cpu.bus_data++;
			if(m68ki_cpu.fast_wmap[a >> 12])
				m68k_fast_mirror(a, w, 2);
		}
		return;
	}
#endif
	m68kf_w16_slow(a, v);
}

/* Two word writes, as m68k_write_memory_32() makes them (and counts them). */
M68KI_HOT2 static __attribute__((noinline)) void m68kf_w32(uint a, uint v)
{
#ifdef AMIGA_FETCH_WINDOW
	if(m68ki_cpu.fast[a >> 16].wr)
	{
		m68kf_w16(a, v >> 16);
		m68kf_w16(ADDRESS_68K(a + 2), v & 0xffff);
		return;
	}
#endif
	m68kf_w32_slow(a, v);
}

/* Slot 0 of the handler table (tools/gen_musashi.py): the illegal instruction
 * and its cycles, the opcode table's (the other handlers' are one number each,
 * m68ki_handler_cycles[]; this one's entry there is 0). */
static void m68ki_op_illegal_cycles(void)
{
	m68k_op_illegal();
	USE_CYCLES(CYC_INSTRUCTION[REG_IR]);
}

#ifdef M68KI_REGVARS
#define M68KI_RUN_ENTER() M68KI_REGS_ENTER(); m68ki_rcpu = m68ki_cpu_mem(); m68ki_rir = m68ki_cpu.ir; M68KI_LOAD()
#define M68KI_RUN_LEAVE() M68KI_SAVE(); M68KI_REGS_LEAVE()
#else
#define M68KI_RUN_ENTER() ((void)0)
#define M68KI_RUN_LEAVE() ((void)0)
#endif

/* One instruction: fetch, the handler, its cycles. */
#ifdef AMIGA_STATS
#define M68KI_RUN_STATS(h) do { \
	amiga_stat_hist[h]++;    /* the bench's handler histogram (tools/musashi_hist.py) */ \
	if (amiga_pc_hist) amiga_pc_hist[(REG_PPC >> 1) & 0xFFFFF]++;   /* ...and by PC (--pchist) */ \
} while (0)
#else
#define M68KI_RUN_STATS(h) ((void)0)
#endif
#define M68KI_RUN_ONE() do { \
	unsigned h; \
	REG_PPC = REG_PC; \
	REG_IR = m68ki_read_imm_16(); \
	h = m68ki_handler_index[REG_IR]; \
	M68KI_RUN_STATS(h); \
	m68ki_handlers[h](); \
	USE_CYCLES(m68ki_handler_cycles[h]); \
} while (0)

/* The loop itself: instructions until the cycles run out; returns how many.
 * On the board it is the emulated CPU's every instruction, so there it is
 * written out (18 instructions a turn; GCC's from the C below: 25).  Registers:
 * r4 the opcode -> handler index, r5 the handlers, r6 this handler's index,
 * r7 the count; r8-r11 are m68kregs.h's.  The structure's offsets are checked. */
#if defined(M68KI_REGVARS) && defined(__arm__) && defined(__thumb2__) && defined(AMIGA_FETCH_WINDOW) \
    && !defined(AMIGA_STATS) && !defined(M68K_NO_ASM_LOOP)
#include <stddef.h>
#define M68KI_OFF_PPC   132
#define M68KI_OFF_FPTR  540
#define M68KI_OFF_FLO   544
_Static_assert(offsetof(m68ki_cpu_core, ppc) == M68KI_OFF_PPC, "m68ki_loop: ppc");
_Static_assert(offsetof(m68ki_cpu_core, fetch_ptr) == M68KI_OFF_FPTR, "m68ki_loop: fetch_ptr");
_Static_assert(offsetof(m68ki_cpu_core, fetch_lo) == M68KI_OFF_FLO, "m68ki_loop: fetch_lo");
_Static_assert(offsetof(m68ki_cpu_core, fetch_len) == M68KI_OFF_FLO + 4, "m68ki_loop: fetch_len");
#define M68KI_STR_(x) #x
#define M68KI_STR(x) M68KI_STR_(x)
M68KI_HOT2 __attribute__((naked, noinline)) static unsigned m68ki_loop(void)
{
	__asm__(
	"	push	{r4, r5, r6, r7, r12, lr}\n"                /* six: the stack stays 8-aligned */
	"	ldr	r4, =m68ki_handler_index\n"
	"	ldr	r5, =m68ki_handlers\n"
	"	movs	r7, #0\n"
	"1:	ldrd	r0, r1, [r11, #" M68KI_STR(M68KI_OFF_FLO) "]\n"   /* the fetch window: lo, len */
	"	bic	r2, r10, #0xff000000\n"
	"	str	r10, [r11, #" M68KI_STR(M68KI_OFF_PPC) "]\n"
	"	subs	r2, r2, r0\n"
	"	add	r10, r10, #2\n"
	"	cmp	r2, r1\n"
	"	bhs	3f\n"
	"	ldr	r0, [r11, #" M68KI_STR(M68KI_OFF_FPTR) "]\n"
	"	ldrh	r0, [r0, r2]\n"
	"	rev16	r8, r0\n"
	"2:	ldrh	r6, [r4, r8, lsl #1]\n"
	"	adds	r7, #1\n"
	"	ldr	r0, [r5, r6, lsl #2]\n"
	"	blx	r0\n"
	"	ldr	r0, =m68ki_handler_cycles\n"
	"	ldrb	r0, [r0, r6]\n"
	"	subs	r9, r9, r0\n"
	"	bgt	1b\n"
	"	mov	r0, r7\n"
	"	pop	{r4, r5, r6, r7, r12, pc}\n"
	"3:	sub	r0, r10, #2\n"                              /* outside the window: through mem.c */
	"	bic	r0, r0, #0xff000000\n"
	"	bl	m68kf_fetch\n"
	"	mov	r8, r0\n"
	"	b	2b\n"
	"	.ltorg\n"
	);
}
#else
M68KI_HOT2 static unsigned m68ki_loop(void)
{
	unsigned instr = 0;
	do
	{
		M68KI_RUN_ONE();
		instr++;
	} while(GET_CYCLES() > 0);
	return instr;
}
#endif

/* Until the cycles run out, the trace flags clear (m68ki_trace_changed() ends
 * it when an instruction sets one; what that instruction traced itself - T0,
 * a change of flow - is taken on the way out). */
M68KI_HOT2 void M68KI_SET(m68ki_run)(void)
{
	unsigned instr;
	M68KI_RUN_ENTER();
	instr = m68ki_loop();
#ifdef AMIGA_FETCH_WINDOW
	amiga_bus_instr += instr;
#else
	(void)instr;
#endif
	m68ki_exception_if_trace(); /* auto-disable (see m68kcpu.h) */
	M68KI_RUN_LEAVE();
}

/* Musashi's loop as it was, for as long as a trace flag is set. */
void M68KI_SET(m68ki_run_traced)(void)
{
	unsigned instr = 0;
	M68KI_RUN_ENTER();
	while((FLAG_T1 | FLAG_T0) && GET_CYCLES() > 0)
	{
		m68ki_trace_t1(); /* auto-disable (see m68kcpu.h) */
		M68KI_RUN_ONE();
		instr++;
		m68ki_exception_if_trace(); /* auto-disable (see m68kcpu.h) */
	}
#ifdef AMIGA_FETCH_WINDOW
	amiga_bus_instr += instr;
#else
	(void)instr;
#endif
	M68KI_RUN_LEAVE();
}

#endif /* M68KRUN__HEADER */
