// Busy-wait loops the CPU need not run (amiga.c's cpu_until, once a slice).
//
// State of the Art's CPU time, by PC (the host bench): over 40% of its
// instructions were loops like these, and a faster CPU (the A1200 load test)
// only spins them more often:
//
//   T: cmpi.w #$236,$45330.l      T: btst #5,$dff01f.l       T: dbra d7,T
//      bcs.s  T                      beq.s T
//
// dbra Dn,* is skipped exactly: as many passes as the slice holds come off Dn
// (10 cycles each; the last, falling through, is left to the CPU).
//
// A test + Bcc back to it (cmpi / btst #n / tst, on abs.w, abs.l or (d16,An))
// reads one value and changes nothing.  When the CPU ends two slices in a row
// inside such a loop with the value unchanged, the following slices are not
// run while the value stays the same and no interrupt is pending: re-read each
// slice, the value tells when the loop would have left.  What may change it -
// an interrupt handler, the blitter, a disk DMA, the beam - acts between slices
// here anyway, so the loop leaves at most one slice later than it would have.
// Only values with no side effect on read are watched: RAM and ROM, the custom
// registers DMACONR, VPOSR, VHPOSR, JOYxDAT, POTGOR, INTENAR, INTREQR, and the
// CIAs' port registers (never ICR, which a read clears).
#include "amiga_int.h"
#include "m68k.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"     // Musashi's header, not ours
#include "m68kcpu.h"
#pragma GCC diagnostic pop

static struct {
    uint32_t t, b;          // the loop: its test at t, its Bcc at b
    uint32_t addr, val;
    uint8_t  size;          // 1, 2, 4 bytes
    uint8_t  seen;          // slice ends found in it with the value unchanged
} lp;
static uint32_t rej_pc = 1;  // the last PC that is no loop (odd: none)

static inline uint16_t op16(uint32_t a) { return (uint16_t)m68k_read_memory_16(a); }

// The test at t: its length and the address it reads; false if not one of ours.
static bool test_at(uint32_t t, uint32_t *len, uint32_t *addr, uint8_t *size) {
    const uint16_t op = op16(t);
    uint32_t l = 2;
    uint8_t sz;
    if ((op & 0xFF00) == 0x0C00 && (op & 0xC0) != 0xC0) {           // CMPI.s #imm,<ea>
        sz = (uint8_t)(1u << ((op >> 6) & 3));
        l += sz == 4 ? 4 : 2;
    } else if ((op & 0xFFC0) == 0x0800) {                           // BTST #n,<ea>
        sz = 1;
        l += 2;
    } else if ((op & 0xFF00) == 0x4A00 && (op & 0xC0) != 0xC0) {    // TST.s <ea>
        sz = (uint8_t)(1u << ((op >> 6) & 3));
    } else {
        return false;
    }
    const unsigned mode = (op >> 3) & 7, reg = op & 7;
    uint32_t a;
    if (mode == 5) {                                                // (d16,An)
        a = m68ki_cpu.dar[8 + reg] + (uint32_t)(int32_t)(int16_t)op16(t + l);
        l += 2;
    } else if (mode == 7 && reg == 0) {                             // abs.w
        a = (uint32_t)(int32_t)(int16_t)op16(t + l);
        l += 2;
    } else if (mode == 7 && reg == 1) {                             // abs.l
        a = (uint32_t)op16(t + l) << 16 | op16(t + l + 2);
        l += 4;
    } else {
        return false;
    }
    *len = l; *addr = a & 0xFFFFFF; *size = sz;
    return true;
}

// Bcc.s back to t at b (not BRA / BSR, whose loop the test cannot end).
static bool bcc_back(uint32_t b, uint32_t t) {
    const uint16_t op = op16(b);
    if ((op & 0xF000) != 0x6000 || ((op >> 8) & 0xF) < 2) return false;
    const int d = (int8_t)(op & 0xFF);
    return d < 0 && b + 2 + (uint32_t)d == t;
}

AMIGA_HOT bool idle_skip(uint64_t target) {
    if (A.cpu_cyc >= target) return true;                           // the last instruction ran past it
    const uint32_t pc = m68ki_cpu.pc & 0xFFFFFF;
    if (m68ki_cpu.stopped || m68ki_cpu.t1_flag) return false;
    const unsigned lvl = m68ki_cpu.int_level;
    if (lvl == 0x700 || lvl > m68ki_cpu.int_mask) { lp.seen = 0; return false; }

    // dbra Dn,* (51C8+n FFFE): as many 10-cycle passes as fit, off Dn.
    const uint16_t op = pc == rej_pc ? 0 : op16(pc);
    if ((op & 0xFFF8) == 0x51C8 && op16(pc + 2) == 0xFFFE) {
        const unsigned mul = A.load_cpu > 1 ? A.load_cpu : 1;
        uint *dn = &m68ki_cpu.dar[op & 7];
        const uint32_t cnt = *dn & 0xFFFF;                          // passes before it falls through
        const uint64_t avail = (target - A.cpu_cyc) * mul / 10;
        uint32_t k = cnt < avail ? cnt : (uint32_t)avail;
        if (k) {
            *dn = (*dn & 0xFFFF0000u) | ((cnt - k) & 0xFFFF);
            const uint32_t c = k * 10 + A.load_rem;
            A.cpu_cyc += c / mul;
            A.load_rem = (uint8_t)(c % mul);
            A.idle_cycles += c / mul;
        }
        return A.cpu_cyc >= target;
    }

    // A test + Bcc loop, at its test or at its branch.
    if (lp.seen && (pc == lp.t || pc == lp.b)) {
        uint32_t v;
        if (mem_peek(lp.addr, lp.size, &v) && v == lp.val) {
            if (lp.seen >= 2) {                                     // still waiting: skip the slice
                A.idle_cycles += (uint32_t)(target - A.cpu_cyc);
                A.cpu_cyc = target;
                return true;
            }
            lp.seen++;
            return false;
        }
        lp.seen = 0;
        return false;
    }
    lp.seen = 0;
    if (pc == rej_pc) return false;
    uint32_t t = pc, len, addr;
    uint8_t size;
    if ((op & 0xF000) == 0x6000) {                                  // at the branch: the test before it
        const int d = (int8_t)(op & 0xFF);
        if (d >= 0 || d < -10) { rej_pc = pc; return false; }
        t = pc + 2 + (uint32_t)d;
    }
    if (!test_at(t, &len, &addr, &size) || !bcc_back(t + len, t) || (pc != t && pc != t + len) ||
        !mem_peek(addr, size, &lp.val)) {
        rej_pc = pc;
        return false;
    }
    lp.t = t; lp.b = t + len; lp.addr = addr; lp.size = size; lp.seen = 1;
    return false;
}

// Snapshots and resets: start over.
void idle_reset(void) { lp.seen = 0; rej_pc = 1; }
