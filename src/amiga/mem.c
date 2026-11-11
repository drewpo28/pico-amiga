// 24-bit address map of an A500 / A500+ / A600 and the Musashi memory callbacks.
//
// The 16 MB space is cut into 256 banks of 64 KB.  A bank is either DIRECT
// (a host pointer + mask, so mirrors fall out of the mask) or an I/O bank
// handled by a small switch.  Chip/slow RAM and ROM are direct, so the CPU's
// common path is: one table load, one masked add, one 16-bit load + byteswap.
//
//   $000000-$1FFFFF  chip RAM (mirrored); ROM overlay over $000000-$07FFFF
//   $A00000-$BFFFFF  CIAs (CIA-A on odd bytes when A12=0, CIA-B on even when A13=0)
//   $C00000-$C7FFFF  slow RAM if fitted, otherwise custom-register mirror
//   $C80000-$D7FFFF  custom-register mirror
//   $DC0000-$DCFFFF  battery clock if the platform has a time (cfg.rtc_time)
//   With Gayle (A600, cfg.gayle): CIAs at $BF0000 only, nothing at $A00000-$BEFFFF
//   (the PCMCIA slot's space, empty) or $C00000-$D7FFFF, Gayle at $D80000-$DDFFFF,
//   its ID at $DD0000-$DEFFFF, no clock ($DC0000 reads 0) - WinUAE's map
//   With Akiko (the CD32, cfg.akiko): Akiko at $B80000-$B8FFFF, no clock
//   $DF0000-$DFFFFF  custom chips
//   $E00000-$E7FFFF  extended ROM (AROS) if supplied
//   $E80000-$EFFFFF  Zorro II autoconfig space: the fast RAM board (cfg.fast_size)
//                    until Kickstart configures it, then empty (reads 0)
//   $200000-$9FFFFF  that board's fast RAM, where Kickstart put it
//   $F80000-$FFFFFF  Kickstart ROM (256 KB images appear twice)
#include <string.h>
#include "amiga_int.h"
#include "m68k.h"

enum { B_NONE = 0, B_CIA, B_CUSTOM, B_RTC, B_GAYLE, B_GAYLE_ID, B_AUTOCONF, B_AKIKO };

typedef struct {
    const uint8_t *rd;   // direct read base, or NULL
    uint8_t       *wr;   // direct write base, or NULL (writes to ROM are dropped)
    uint32_t       mask; // read offset mask applied to the full address
    uint32_t       wmask;// write offset mask (differs from mask under the ROM overlay)
    uint8_t        io;   // B_* when rd == NULL
    uint8_t        chip; // on the chip bus (chip RAM, the custom registers): the CPU waits for DMA there
} bank_t;

static bank_t banks[256];
static bool   overlay;

// What the opcode handlers reach themselves (m68k.h, m68krun.h): a direct bank
// as the host address of its address 0, for reads and for writes.  A bank is in
// it when its block is 64 KB or more (so the mask only folds the bank number)
// and, as the handlers count an access below M68K_FAST_CHIP_TOP for the chip bus
// and no other, when it is chip RAM exactly there.  Kept current by fast_bank().
static void fast_bank(int i) {
    const bank_t *b = &banks[i];
    const uint32_t base = (uint32_t)i << 16;
    const bool ok = (b->chip != 0) == (base < M68K_FAST_CHIP_TOP);
    m68k_fast[i].rd = b->rd && b->mask >= 0xFFFF && ok
        ? (const uint8_t *)((uintptr_t)b->rd + (base & b->mask) - base) : NULL;
    m68k_fast[i].wr = b->wr && b->wmask >= 0xFFFF && ok
        ? (uint8_t *)((uintptr_t)b->wr + (base & b->wmask) - base) : NULL;
}

// The copper list mirror's share of a write the handlers made (m68k_fast_wmap).
AMIGA_HOT void m68k_fast_mirror(unsigned int a, unsigned int v, unsigned int size) {
    if (size == 1) cop_mir_w8(a & A.chip_mask, (uint8_t)v);
    else cop_mir_w16(a & A.chip_mask, (uint16_t)v);
}

// Musashi's instruction fetch window (m68kcpu.h, AMIGA_FETCH_WINDOW, set with
// m68k_set_fetch_window): the block the code last ran in (chip RAM, a ROM: the
// run of 64 KB banks that are one copy of it), read straight from its memory.
// Any change to banks[] closes it.
uint8_t mem_fetch_chip;        // ...and it is a chip bank (the chip bus model)
uint8_t mem_fetch_fast;        // ...or the fast RAM board's (amiga_set_fast_throttle)

static void map(int first, int count, const uint8_t *rd, uint8_t *wr, uint32_t mask, int io) {
    m68k_set_fetch_window(NULL, 0, 0);
    for (int i = first; i < first + count && i < 256; i++) {
        banks[i].rd = rd; banks[i].wr = wr; banks[i].mask = mask; banks[i].wmask = mask;
        banks[i].io = (uint8_t)io;
        banks[i].chip = (rd && rd == A.cfg.chip_ram) || io == B_CUSTOM;
        fast_bank(i);
    }
}

static void map_low(void) {
    const amiga_config_t *c = &A.cfg;
    // Chip RAM and its mirrors up to 2 MB.
    map(0x00, 0x20, c->chip_ram, c->chip_ram, c->chip_size - 1, B_NONE);
    m68k_set_fetch_window(NULL, 0, 0);
    if (overlay) {
        // ROM reads over the bottom 512 KB; writes still land in chip RAM.
        for (int i = 0; i < 8; i++) {
            banks[i].rd = c->rom;
            banks[i].mask = c->rom_size - 1;     // wr/wmask stay chip RAM
            fast_bank(i);
        }
    }
}

// ---- the Zorro II fast RAM board (WinUAE's expansion.cpp) ------------------------
// Its autoconfig ROM at $E80000: each register byte as two nibbles, in the high
// halves of the bytes at $00/$02, $04/$06, ..., inverted but for
// er_Type ($00) and the base address registers ($40-$42).  Kickstart writes the
// base's low nibble to $4A, then its high byte to $48, which configures the board;
// $4C shuts it up.  Unconfigured by any reset (mem_init) and the RESET
// instruction, as boards are.
static uint8_t  z2_rom[128];
static bool     z2_done;            // configured or shut up: the space reads empty
static uint8_t  z2_lo;
static uint32_t z2_base;            // 0: not mapped

static void z2_reg(int r, uint8_t v) {
    const bool plain = r == 0x00 || r == 0x40;
    z2_rom[r]     = plain ? (uint8_t)(v & 0xF0) : (uint8_t)~(v & 0xF0);
    z2_rom[r + 2] = plain ? (uint8_t)((v & 0x0F) << 4) : (uint8_t)~((v & 0x0F) << 4);
}

static void z2_init(void) {
    const uint32_t size = A.cfg.fast_ram ? A.cfg.fast_size : 0;
    uint8_t code = 0;                                        // 8 MB
    if (size == 1u << 20) code = 5; else if (size == 2u << 20) code = 6; else if (size == 4u << 20) code = 7;
    memset(z2_rom, 0xFF, sizeof z2_rom);
    z2_reg(0x00, (uint8_t)(0xC0 | 0x20 | code));             // Zorro II, add to the free list, size
    z2_reg(0x04, 0x51);                                      // er_Product
    z2_reg(0x08, 0x00);                                      // er_Flags
    z2_reg(0x10, 0x07); z2_reg(0x14, 0xDB);                  // er_Manufacturer (WinUAE's 2011)
    for (int r = 0x18; r <= 0x2C; r += 4) z2_reg(r, 0x00);   // serial number, no diag ROM
    z2_done = !size;
    z2_lo = 0;
}

static void z2_unmap(void) {
    if (!z2_base) return;
    const uint32_t size = A.cfg.fast_size;
    map((int)(z2_base >> 16), (int)(size >> 16), NULL, NULL, 0, B_NONE);
    z2_base = 0;
}

static void z2_map(uint32_t base) {
    const uint32_t size = A.cfg.fast_size;
    if (base < 0x200000 || base + size > 0xA00000) return;   // Zorro II's 8 MB space only
    z2_base = base;
    map((int)(base >> 16), (int)(size >> 16), A.cfg.fast_ram, A.cfg.fast_ram, size - 1, B_NONE);
}

void mem_autoconfig_reset(void) {
    z2_unmap();
    z2_init();
}

static uint8_t z2_read8(uint32_t a) {
    return z2_done ? 0 : z2_rom[a & 0x7F];
}

static void z2_write8(uint32_t a, uint8_t v) {
    if (z2_done) return;
    switch (a & 0xFF) {
    case 0x4A: z2_lo = v; break;
    case 0x48:
        z2_map((uint32_t)(v | (z2_lo >> 4)) << 16);
        z2_done = true;
        break;
    case 0x4C: z2_done = true; break;                        // shut up
    default: break;
    }
}

void mem_init(void) {
    const amiga_config_t *c = &A.cfg;
    memset(banks, 0, sizeof banks);
    memset(m68k_fast, 0, 256 * sizeof m68k_fast[0]);
    map(0xDF, 0x01, NULL, NULL, 0, B_CUSTOM);
    if (c->gayle) {
        map(0xBF, 0x01, NULL, NULL, 0, B_CIA);
        map(0xD8, 0x06, NULL, NULL, 0, B_GAYLE);
        map(0xDC, 0x01, NULL, NULL, 0, B_NONE);        // no clock fitted
        map(0xDD, 0x02, NULL, NULL, 0, B_GAYLE_ID);
    } else {
        map(0xA0, 0x20, NULL, NULL, 0, B_CIA);
        map(0xC0, 0x18, NULL, NULL, 0, B_CUSTOM);      // $C00000-$D7FFFF mirror
        if (c->rtc_time && !c->akiko) map(0xDC, 0x01, NULL, NULL, 0, B_RTC);
        if (c->akiko) map(0xB8, 0x01, NULL, NULL, 0, B_AKIKO);
    }
    if (c->slow_ram && c->slow_size)
        map(0xC0, (int)(c->slow_size >> 16), c->slow_ram, c->slow_ram, c->slow_size - 1, B_NONE);
    if (c->ext_rom && c->ext_size)
        map(0xE0, 0x08, c->ext_rom, NULL, c->ext_size - 1, B_NONE);
    map(0xF8, 0x08, c->rom, NULL, c->rom_size - 1, B_NONE);
    z2_base = 0;
    z2_init();
    if (c->fast_ram && c->fast_size) map(0xE8, 0x01, NULL, NULL, 0, B_AUTOCONF);
    overlay = true;
    map_low();
}

void mem_set_overlay(bool on) {
    if (on == overlay) return;
    overlay = on;
    map_low();
}

bool mem_overlay(void) { return overlay; }

// Snapshots: the map is the configuration's (mem_init); the overlay moves, and
// the fast RAM board's configuration (at the end: older snapshots read it
// unconfigured).
void mem_state(st_t *s) {
    bool ov = overlay;
    st_bool(s, &ov);
    if (s->mode == ST_LOAD) mem_set_overlay(ov);
    bool done = z2_done;
    uint32_t base = z2_base;
    st_bool(s, &done); st_u8(s, &z2_lo); st_u32(s, &base);
    if (s->mode == ST_LOAD) {
        z2_unmap();
        z2_init();
        if (A.cfg.fast_ram && A.cfg.fast_size) {
            if (base) z2_map(base);
            z2_done = done;
        }
    }
}

// ---- I/O banks --------------------------------------------------------------
AMIGA_HOT static uint8_t io_read8(uint32_t a, int io) {
    switch (io) {
    case B_CIA:
        if (a & 1) return (a & 0x1000) ? 0xFF : cia_read(0, (a >> 8) & 0xF);
        return (a & 0x2000) ? 0xFF : cia_read(1, (a >> 8) & 0xF);
    case B_CUSTOM: {
        uint16_t w = custom_read(a & 0x1FE);
        return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
    }
    case B_RTC:                     // register n: the low nibble of the word at $DC0000 + 4n + 2
        return (a & 1) ? rtc_read((int)(a >> 2) & 15) : 0;
    case B_GAYLE:
        return gayle_read(a);
    case B_GAYLE_ID:
        return gayle_id_read(a);
    case B_AUTOCONF:
        return z2_read8(a);
    case B_AKIKO:
        return akiko_read(a);
    default:
        return 0;
    }
}

AMIGA_HOT static uint16_t io_read16(uint32_t a, int io) {
    switch (io) {
    case B_CIA: {
        uint8_t hi = (a & 0x2000) ? 0xFF : cia_read(1, (a >> 8) & 0xF);
        uint8_t lo = (a & 0x1000) ? 0xFF : cia_read(0, (a >> 8) & 0xF);
        return (uint16_t)((hi << 8) | lo);
    }
    case B_CUSTOM:
        return custom_read(a & 0x1FE);
    case B_RTC:
        return rtc_read((int)(a >> 2) & 15);
    case B_GAYLE:
        return gayle_read16(a);
    case B_AKIKO:
        return akiko_read16(a);
    case B_GAYLE_ID:                // a byte at a time, the high one first, as WinUAE
    case B_AUTOCONF:
        return (uint16_t)((io_read8(a, io) << 8) | io_read8(a + 1, io));
    default:
        return 0;
    }
}

AMIGA_HOT static void io_write8(uint32_t a, uint8_t v, int io) {
    switch (io) {
    case B_CIA:
        if (a & 1) { if (!(a & 0x1000)) cia_write(0, (a >> 8) & 0xF, v); }
        else       { if (!(a & 0x2000)) cia_write(1, (a >> 8) & 0xF, v); }
        break;
    case B_CUSTOM:
        // The 68000 drives a byte write onto both halves of the data bus.
        custom_write(a & 0x1FE, (uint16_t)((v << 8) | v), amiga_hpos(), false);
        break;
    case B_RTC:
        if (a & 1) rtc_write((int)(a >> 2) & 15, v);
        break;
    case B_GAYLE:
        gayle_write(a, v);
        break;
    case B_GAYLE_ID:
        gayle_id_write();
        break;
    case B_AUTOCONF:
        z2_write8(a, v);
        break;
    case B_AKIKO:
        akiko_write(a, v);
        break;
    default:
        break;
    }
}

AMIGA_HOT static void io_write16(uint32_t a, uint16_t v, int io) {
    switch (io) {
    case B_CIA:
        if (!(a & 0x2000)) cia_write(1, (a >> 8) & 0xF, (uint8_t)(v >> 8));
        if (!(a & 0x1000)) cia_write(0, (a >> 8) & 0xF, (uint8_t)v);
        break;
    case B_CUSTOM:
        custom_write(a & 0x1FE, v, amiga_hpos(), false);
        break;
    case B_RTC:
        rtc_write((int)(a >> 2) & 15, (uint8_t)v);
        break;
    case B_GAYLE:
        gayle_write16(a, v);
        break;
    case B_AKIKO:
        akiko_write16(a, v);
        break;
    case B_GAYLE_ID:
        io_write8(a, (uint8_t)(v >> 8), io);
        io_write8(a + 1, (uint8_t)v, io);
        break;
    case B_AUTOCONF:                // the board's registers sit on the even byte
        z2_write8(a, (uint8_t)(v >> 8));
        break;
    default:
        break;
    }
}

// A read with no side effect (idle.c): RAM / ROM, and the I/O registers that
// only report state.  False for anything else (CIA ICR, COPJMP, Gayle, ...).
static bool custom_pure(uint32_t reg) {
    switch (reg) {
    case 0x002: case 0x004: case 0x006: case 0x00A: case 0x00C:      // DMACONR VPOSR VHPOSR JOYxDAT
    case 0x016: case 0x01C: case 0x01E: return true;                  // POTGOR INTENAR INTREQR
    default: return false;
    }
}
AMIGA_HOT bool mem_peek(uint32_t a, int size, uint32_t *v) {
    a &= 0xFFFFFF;
    const bank_t *b = &banks[a >> 16];
    if (b->rd) {
        const uint8_t *p = b->rd;
        const uint32_t m = b->mask;
        uint32_t x = 0;
        for (int i = 0; i < size; i++) x = x << 8 | p[(a + (uint32_t)i) & m];
        *v = x;
        return true;
    }
    for (int i = 0; i < size; i += 2) {
        const uint32_t w = (a + (uint32_t)i) & ~1u;
        if (b->io == B_CUSTOM ? !custom_pure(w & 0x1FE)
          : b->io == B_CIA ? ((w >> 8) & 0xF) > 1                       // the CIAs' PRA / PRB only
          : true) return false;
    }
    if (size == 1) *v = io_read8(a, b->io);
    else if (size == 2) *v = io_read16(a, b->io);
    else *v = (uint32_t)io_read16(a, b->io) << 16 | io_read16(a + 2, b->io);
    return true;
}

// ---- Musashi callbacks ------------------------------------------------------
static inline uint16_t ld16(const uint8_t *p) {
    uint16_t v; memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}
static inline void st16(uint8_t *p, uint16_t v) {
    v = __builtin_bswap16(v); memcpy(p, &v, 2);
}

AMIGA_HOT unsigned int m68k_read_memory_8(unsigned int a) {
    a &= 0xFFFFFF;
    AMIGA_STAT(amiga_stat_data);
    const bank_t *b = &banks[a >> 16];
    A.bus_data += b->chip;
    if (b->rd) return b->rd[a & b->mask];
    return io_read8(a, b->io);
}

AMIGA_HOT unsigned int m68k_read_memory_16(unsigned int a) {
    a &= 0xFFFFFF;
    AMIGA_STAT(amiga_stat_data);
    const bank_t *b = &banks[a >> 16];
    A.bus_data += b->chip;
    if (b->rd) return ld16(b->rd + (a & b->mask));
    return io_read16(a, b->io);
}

// The fetch window moved to the block `a` is in (m68k.h; the handlers' PC left
// the old one), if it is one: RAM or ROM of 64 KB or more.  The move is the
// CPU's access to that memory - counted for the chip bus, as the read of the
// first word there was.
AMIGA_HOT void m68k_fetch_window(unsigned int a) {
    a &= 0xFFFFFF;
    const bank_t *b = &banks[a >> 16];
    AMIGA_STAT(amiga_stat_data);
    A.bus_data += b->chip;
    if (b->rd && b->mask >= 0xFFFF) {
        // The window: this bank and its neighbours of the same block (one copy of
        // it: a mirror is another window), so code that calls across 64 KB - 9%
        // of Roots' 8-plane part's instructions came through here - stays in it.
        const uint32_t blk = a & ~b->mask;
        uint32_t first = a >> 16, last = first;
#define SAME(j) (banks[j].rd == b->rd && banks[j].mask == b->mask && banks[j].chip == b->chip && \
                 (((uint32_t)(j) << 16) & ~b->mask) == blk)
        while (first > 0 && SAME(first - 1)) first--;
        while (last < 255 && SAME(last + 1)) last++;
#undef SAME
        const uint32_t lo = first << 16;
        m68k_set_fetch_window(b->rd + (lo & b->mask), lo, (last - first + 1) << 16);
        mem_fetch_chip = b->chip;
        mem_fetch_fast = A.cfg.fast_ram && b->rd == A.cfg.fast_ram;
    }
}

// An instruction's bytes where the window cannot give them (m68k.h): the end of
// a block, memory that is not there.  No side effects: I/O reads as 0.
void m68k_fetch_stage(unsigned int a, unsigned char *buf, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        const uint32_t x = (a + i) & 0xFFFFFF;
        const bank_t *b = &banks[x >> 16];
        buf[i] = b->rd ? b->rd[x & b->mask] : 0;
    }
}

// Musashi's own fetch (a build without the pointer PC, m68kcpu.h).
AMIGA_HOT unsigned int m68ki_fetch_slow16(unsigned int a) {
    a &= 0xFFFFFF;
    const bank_t *b = &banks[a >> 16];
    const bool chip = b->chip;
    m68k_fetch_window(a);
    A.bus_data -= chip;                                  // m68k_read_memory_16 counts it
#ifdef AMIGA_STATS
    amiga_stat_data--;
#endif
    return m68k_read_memory_16(a);
}

AMIGA_HOT unsigned int m68k_read_memory_32(unsigned int a) {
    a &= 0xFFFFFF;
#ifdef AMIGA_TRACE_VECTORS
    // A vector fetch of exceptions 2-11 (host debugging): the faulting PC and opcode.
    if (a >= 8 && a < 48 && !(a & 3)) {
        extern int printf(const char *, ...);
        printf("[vec %u] ppc=%06X pc=%06X ir=%04X sr=%04X frame %u\n", a / 4,
               m68k_get_reg(NULL, M68K_REG_PPC), m68k_get_reg(NULL, M68K_REG_PC),
               m68k_get_reg(NULL, M68K_REG_IR), m68k_get_reg(NULL, M68K_REG_SR), (unsigned)A.frame);
        const uint32_t ppc = m68k_get_reg(NULL, M68K_REG_PPC);
        if (a == 16 && ppc >= 32 && ppc < A.cfg.chip_size - 64) {
            printf("[vec]  code:");
            for (uint32_t x = ppc - 24; x < ppc + 40; x += 2) printf("%s%04X", x == ppc ? " *" : " ", m68k_read_memory_16(x));
            printf("\n");
        }
    }
#endif
    AMIGA_STAT(amiga_stat_data);
    const bank_t *b = &banks[a >> 16];
    A.bus_data += b->chip;
    if (b->rd && ((a + 2) & 0xFFFF) >= 2) {           // both halves in this bank
        const uint8_t *p = b->rd + (a & b->mask);
        return ((uint32_t)ld16(p) << 16) | ld16(p + 2);
    }
    if (!b->rd && b->io == B_AKIKO) return akiko_read32(a);     // one access: the C2P counts them
    uint32_t hi = m68k_read_memory_16(a);
    return (hi << 16) | m68k_read_memory_16(a + 2);
}

AMIGA_HOT void m68k_write_memory_8(unsigned int a, unsigned int v) {
    a &= 0xFFFFFF;
    AMIGA_STAT(amiga_stat_data);
    const bank_t *b = &banks[a >> 16];
    A.bus_data += b->chip;
    if (b->wr) {
        b->wr[a & b->wmask] = (uint8_t)v;
        if (b->chip) cop_mir_w8(a & b->wmask, (uint8_t)v);   // the copper list mirror
        return;
    }
    if (!b->rd) io_write8(a, (uint8_t)v, b->io);
}

AMIGA_HOT void m68k_write_memory_16(unsigned int a, unsigned int v) {
    a &= 0xFFFFFF;
    AMIGA_STAT(amiga_stat_data);
    const bank_t *b = &banks[a >> 16];
    A.bus_data += b->chip;
    if (b->wr) {
        st16(b->wr + (a & b->wmask), (uint16_t)v);
        if (b->chip) cop_mir_w16(a & b->wmask, __builtin_bswap16((uint16_t)v));
        return;
    }
    if (!b->rd) io_write16(a, (uint16_t)v, b->io);
}

AMIGA_HOT void m68k_write_memory_32(unsigned int a, unsigned int v) {
    if (banks[(a >> 16) & 0xFF].io == B_AKIKO && !banks[(a >> 16) & 0xFF].rd) { akiko_write32(a & 0xFFFFFF, v); return; }
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}

// Disassembler reads (host debugging only) must not have side effects.
unsigned int m68k_read_disassembler_16(unsigned int a) {
    a &= 0xFFFFFF;
    const bank_t *b = &banks[a >> 16];
    return b->rd ? ld16(b->rd + (a & b->mask)) : 0;
}
unsigned int m68k_read_disassembler_32(unsigned int a) {
    return (m68k_read_disassembler_16(a) << 16) | m68k_read_disassembler_16(a + 2);
}
