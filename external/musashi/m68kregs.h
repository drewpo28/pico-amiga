/* PICO-AMIGA: the CPU state the opcode handlers use most, in registers.
 *
 * Included first thing by gen/m68kops.c (tools/gen_musashi.py), before any
 * function is defined: GCC's global register variables.  The handlers and the
 * loop that runs them (m68krun.h) are the only code compiled with them; they
 * see the base of m68ki_cpu, the PC, the cycles left and the instruction word
 * in callee-saved registers, so an instruction loads and stores none of them.
 * Everything else (m68kcpu.c, the emulator) sees the copies in memory, which
 * are current outside m68k_execute() and inside a memory callback (m68krun.h's
 * accessors store the registers before one and load them after).
 *
 * Without them (another compiler or architecture, M68K_NO_REGVARS, the stock
 * RAM tables) the same code runs on the memory copies. */
#ifndef M68KREGS__HEADER
#define M68KREGS__HEADER

#include "m68kconf.h"

#if M68K_CONST_TABLES
#define M68KI_OPS 1          /* this file is a handler set: m68kcpu.h's register view */
/* With the platform's fetch window (AMIGA_FETCH_WINDOW) the handlers' PC is a
 * host pointer into the memory the code runs from (m68kcpu.h "The PC as a host
 * pointer"); without it (Musashi's test driver) it is the 68k address. */
#ifdef AMIGA_FETCH_WINDOW
#define M68KI_PCPTR 1
typedef const unsigned char *m68ki_pc_t;
#else
typedef unsigned int m68ki_pc_t;
#endif
#if !defined(M68K_NO_REGVARS) && defined(__GNUC__) && !defined(__clang__)
struct m68ki_cpu_core_s;
#if defined(__arm__)
#define M68KI_REGVARS 1
register struct m68ki_cpu_core_s *m68ki_rcpu __asm__("r11");
register m68ki_pc_t               m68ki_rpc  __asm__("r10");
register int                      m68ki_rcyc __asm__("r9");
register unsigned int             m68ki_rir  __asm__("r8");
/* The caller's values of the four (they are callee-saved), around the loop. */
#define M68KI_REGS_ENTER() \
	struct m68ki_cpu_core_s *const m68ki_s0 = m68ki_rcpu; \
	const m68ki_pc_t m68ki_s1 = m68ki_rpc; const unsigned int m68ki_s3 = m68ki_rir; const int m68ki_s2 = m68ki_rcyc
#define M68KI_REGS_LEAVE() \
	do { m68ki_rcpu = m68ki_s0; m68ki_rpc = m68ki_s1; m68ki_rcyc = m68ki_s2; m68ki_rir = m68ki_s3; } while (0)
#elif defined(__x86_64__)
#define M68KI_REGVARS 1
register struct m68ki_cpu_core_s *m68ki_rcpu __asm__("r15");
register m68ki_pc_t               m68ki_rpc  __asm__("r14");
register int                      m68ki_rcyc __asm__("r13");
register unsigned int             m68ki_rir  __asm__("r12");
/* 64-bit registers behind 32-bit variables: saved whole. */
#define M68KI_REGS_ENTER() \
	unsigned long long m68ki_s[4]; \
	__asm__ volatile("mov %%r15, 0(%0)\n\tmov %%r14, 8(%0)\n\tmov %%r13, 16(%0)\n\tmov %%r12, 24(%0)" \
	                 : : "r"(m68ki_s) : "memory")
#define M68KI_REGS_LEAVE() \
	__asm__ volatile("mov 0(%0), %%r15\n\tmov 8(%0), %%r14\n\tmov 16(%0), %%r13\n\tmov 24(%0), %%r12" \
	                 : : "r"(m68ki_s) : "memory")
#endif
#endif
#endif /* M68K_CONST_TABLES */

#endif /* M68KREGS__HEADER */
