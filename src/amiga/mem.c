// 24-bit address map of an A500 and the Musashi memory callbacks.
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
//   $DF0000-$DFFFFF  custom chips
//   $E00000-$E7FFFF  extended ROM (AROS) if supplied
//   $E80000-$EFFFFF  Zorro II autoconfig space (empty: reads 0)
//   $F80000-$FFFFFF  Kickstart ROM (256 KB images appear twice)
#include <string.h>
#include "amiga_int.h"
#include "m68k.h"

enum { B_NONE = 0, B_CIA, B_CUSTOM, B_RTC };

typedef struct {
    const uint8_t *rd;   // direct read base, or NULL
    uint8_t       *wr;   // direct write base, or NULL (writes to ROM are dropped)
    uint32_t       mask; // read offset mask applied to the full address
    uint32_t       wmask;// write offset mask (differs from mask under the ROM overlay)
    uint8_t        io;   // B_* when rd == NULL
} bank_t;

static bank_t banks[256];
static bool   overlay;

static void map(int first, int count, const uint8_t *rd, uint8_t *wr, uint32_t mask, int io) {
    for (int i = first; i < first + count && i < 256; i++) {
        banks[i].rd = rd; banks[i].wr = wr; banks[i].mask = mask; banks[i].wmask = mask;
        banks[i].io = (uint8_t)io;
    }
}

static void map_low(void) {
    const amiga_config_t *c = &A.cfg;
    // Chip RAM and its mirrors up to 2 MB.
    map(0x00, 0x20, c->chip_ram, c->chip_ram, c->chip_size - 1, B_NONE);
    if (overlay) {
        // ROM reads over the bottom 512 KB; writes still land in chip RAM.
        for (int i = 0; i < 8; i++) {
            banks[i].rd = c->rom;
            banks[i].mask = c->rom_size - 1;     // wr/wmask stay chip RAM
        }
    }
}

void mem_init(void) {
    const amiga_config_t *c = &A.cfg;
    memset(banks, 0, sizeof banks);
    map(0xA0, 0x20, NULL, NULL, 0, B_CIA);
    map(0xC0, 0x18, NULL, NULL, 0, B_CUSTOM);          // $C00000-$D7FFFF mirror
    map(0xDF, 0x01, NULL, NULL, 0, B_CUSTOM);
    if (c->rtc_time) map(0xDC, 0x01, NULL, NULL, 0, B_RTC);
    if (c->slow_ram && c->slow_size)
        map(0xC0, (int)(c->slow_size >> 16), c->slow_ram, c->slow_ram, c->slow_size - 1, B_NONE);
    if (c->ext_rom && c->ext_size)
        map(0xE0, 0x08, c->ext_rom, NULL, c->ext_size - 1, B_NONE);
    map(0xF8, 0x08, c->rom, NULL, c->rom_size - 1, B_NONE);
    overlay = true;
    map_low();
}

void mem_set_overlay(bool on) {
    if (on == overlay) return;
    overlay = on;
    map_low();
}

bool mem_overlay(void) { return overlay; }

// ---- I/O banks --------------------------------------------------------------
static uint8_t io_read8(uint32_t a, int io) {
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
    default:
        return 0;
    }
}

static uint16_t io_read16(uint32_t a, int io) {
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
    default:
        return 0;
    }
}

static void io_write8(uint32_t a, uint8_t v, int io) {
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
    default:
        break;
    }
}

static void io_write16(uint32_t a, uint16_t v, int io) {
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
    default:
        break;
    }
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
    const bank_t *b = &banks[a >> 16];
    if (b->rd) return b->rd[a & b->mask];
    return io_read8(a, b->io);
}

AMIGA_HOT unsigned int m68k_read_memory_16(unsigned int a) {
    a &= 0xFFFFFF;
    const bank_t *b = &banks[a >> 16];
    if (b->rd) return ld16(b->rd + (a & b->mask));
    return io_read16(a, b->io);
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
    const bank_t *b = &banks[a >> 16];
    if (b->rd && ((a + 2) & 0xFFFF) >= 2) {           // both halves in this bank
        const uint8_t *p = b->rd + (a & b->mask);
        return ((uint32_t)ld16(p) << 16) | ld16(p + 2);
    }
    uint32_t hi = m68k_read_memory_16(a);
    return (hi << 16) | m68k_read_memory_16(a + 2);
}

AMIGA_HOT void m68k_write_memory_8(unsigned int a, unsigned int v) {
    a &= 0xFFFFFF;
    const bank_t *b = &banks[a >> 16];
    if (b->wr) { b->wr[a & b->wmask] = (uint8_t)v; return; }
    if (!b->rd) io_write8(a, (uint8_t)v, b->io);
}

AMIGA_HOT void m68k_write_memory_16(unsigned int a, unsigned int v) {
    a &= 0xFFFFFF;
    const bank_t *b = &banks[a >> 16];
    if (b->wr) { st16(b->wr + (a & b->wmask), (uint16_t)v); return; }
    if (!b->rd) io_write16(a, (uint16_t)v, b->io);
}

AMIGA_HOT void m68k_write_memory_32(unsigned int a, unsigned int v) {
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
