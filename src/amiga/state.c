// Snapshots (amiga.h): the machine's state as blocks, id[4] size(u32 LE) data.
//
// Each module has one function that moves its fields both ways (amiga_int.h,
// st_t); a block is written once to count its size and once for real, so
// nothing is buffered.  A field past a block's end reads 0 (a block from an
// older version that had fewer fields), bytes left over are skipped (a newer
// one with more), and an unknown block is skipped whole.
//
//   MACH  format version, chipset, chip / slow RAM size, ROM CRCs (checked first)
//   SYS   beam, clock, DMACON / INTENA / INTREQ ..., keyboard controller (amiga.c)
//   CPU   the 68000 (Musashi's registers, flags, STOP and interrupt state)
//   MEM   the ROM overlay      CUST  CLXCON       CIAS  both 8520s
//   RTC   clock control        COPR  copper       BLIT  blitter
//   DENI  bitplane / sprite DMA and Denise's registers
//   PAUL  the audio channels   FLOP  drives, disk DMA
//   CHIP  chip RAM             SLOW  slow RAM (when fitted)
//
// Taken between frames only: the Denise stages are idle then and the CPU is
// outside m68k_execute(), so there is nothing in flight but what the modules
// hold.  Cold code (flash on the board): the menu runs it.
#include <string.h>
#include "amiga_int.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"     // Musashi's header, not ours
#include "m68kcpu.h"
#pragma GCC diagnostic pop

#define STATE_VERSION 1

// ---- fields ------------------------------------------------------------------------
void st_raw(st_t *s, void *p, uint32_t n) {
    switch (s->mode) {
    case ST_SIZE:
        s->n += n;
        break;
    case ST_SAVE:
        if (s->ok && n && !s->io->write(s->io->ctx, p, n)) s->ok = false;
        break;
    default: {
        const uint32_t take = n < s->n ? n : s->n;
        if (take && s->io->read(s->io->ctx, p, take) != take) s->ok = false;
        s->n -= take;
        if (take < n) memset((uint8_t *)p + take, 0, n - take);   // past the block: 0
        break;
    }
    }
}

// An integer of `size` bytes, little-endian on the card.
static uint64_t st_le(st_t *s, uint64_t v, int size) {
    uint8_t b[8];
    for (int i = 0; i < size; i++) b[i] = (uint8_t)(v >> (8 * i));
    st_raw(s, b, (uint32_t)size);
    uint64_t r = 0;
    for (int i = 0; i < size; i++) r |= (uint64_t)b[i] << (8 * i);
    return r;
}

// Saving and counting leave the variable as it is; loading stores what was read.
#define ST_FIELD(name, T, size)                                         \
    void st_##name(st_t *s, T *v) {                                     \
        const uint64_t r = st_le(s, (uint64_t)*v, size);                \
        if (s->mode == ST_LOAD) *v = (T)r;                              \
    }
ST_FIELD(u8, uint8_t, 1)
ST_FIELD(i8, int8_t, 1)
ST_FIELD(u16, uint16_t, 2)
ST_FIELD(i16, int16_t, 2)
ST_FIELD(u32, uint32_t, 4)
ST_FIELD(int, int, 4)
ST_FIELD(uint, unsigned, 4)
ST_FIELD(u64, uint64_t, 8)

void st_bool(st_t *s, bool *v) {
    uint8_t b = *v;
    st_u8(s, &b);
    if (s->mode == ST_LOAD) *v = b != 0;
}

// ---- CRC-32 (the ROMs) ------------------------------------------------------------
uint32_t amiga_crc32(uint32_t crc, const void *data, uint32_t n) {
    const uint8_t *p = data;
    uint32_t c = ~crc;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// ---- blocks ------------------------------------------------------------------------
// MACH: what the snapshot needs from the configuration; on load it must match.
static void mach_state(st_t *s) {
    uint16_t ver = STATE_VERSION;
    uint8_t chipset = A.cfg.chipset;
    uint32_t chip = A.cfg.chip_size, slow = A.cfg.slow_ram ? A.cfg.slow_size : 0;
    uint32_t rom = 0, ext = 0;
    if (s->mode != ST_SIZE) {
        rom = amiga_crc32(0, A.cfg.rom, A.cfg.rom_size);
        ext = A.cfg.ext_rom && A.cfg.ext_size ? amiga_crc32(0, A.cfg.ext_rom, A.cfg.ext_size) : 0;
    }
    const uint32_t rom_now = rom, ext_now = ext;
    st_u16(s, &ver); st_u8(s, &chipset);
    st_u32(s, &chip); st_u32(s, &slow);
    st_u32(s, &rom); st_u32(s, &ext);
    if (s->mode == ST_LOAD &&
        (ver != STATE_VERSION || chipset != A.cfg.chipset || chip != A.cfg.chip_size ||
         slow != (A.cfg.slow_ram ? A.cfg.slow_size : 0) || rom != rom_now || ext != ext_now))
        s->ok = false;
}

// The 68000: Musashi's state as m68k_execute() leaves it.  Its cycle tables,
// masks and callbacks are the CPU type's (m68k_set_cpu_type), not the program's.
static void st_reg(st_t *s, uint *v) {
    unsigned u = *v;
    st_uint(s, &u);
    *v = u;
}
static void cpu_state(st_t *s) {
    m68ki_cpu_core *c = &m68ki_cpu;
    for (int i = 0; i < 16; i++) st_reg(s, &c->dar[i]);
    st_reg(s, &c->ppc); st_reg(s, &c->pc);
    for (int i = 0; i < 7; i++) st_reg(s, &c->sp[i]);
    st_reg(s, &c->vbr); st_reg(s, &c->sfc); st_reg(s, &c->dfc);
    st_reg(s, &c->ir);
    st_reg(s, &c->t1_flag); st_reg(s, &c->t0_flag); st_reg(s, &c->s_flag); st_reg(s, &c->m_flag);
    st_reg(s, &c->x_flag); st_reg(s, &c->n_flag); st_reg(s, &c->not_z_flag);
    st_reg(s, &c->v_flag); st_reg(s, &c->c_flag);
    st_reg(s, &c->int_mask); st_reg(s, &c->int_level);
    st_reg(s, &c->stopped);
    st_reg(s, &c->pref_addr); st_reg(s, &c->pref_data);
    st_reg(s, &c->instr_mode); st_reg(s, &c->run_mode);
    st_reg(s, &c->reset_cycles);
    st_reg(s, &c->virq_state); st_reg(s, &c->nmi_pending);
}

static void chip_state(st_t *s) { st_raw(s, A.cfg.chip_ram, A.cfg.chip_size); }
static void slow_state(st_t *s) {
    if (A.cfg.slow_ram && A.cfg.slow_size) st_raw(s, A.cfg.slow_ram, A.cfg.slow_size);
}

// In this order on save; a load takes them in the file's (MACH first).
static const struct { char id[5]; void (*fn)(st_t *); } kBlocks[] = {
    { "MACH", mach_state },   { "SYS ", amiga_sys_state }, { "CPU ", cpu_state },
    { "MEM ", mem_state },    { "CUST", custom_state },    { "CIAS", cia_state },
    { "RTC ", rtc_state },    { "COPR", copper_state },    { "BLIT", blitter_state },
    { "DENI", denise_state }, { "PAUL", paula_state },     { "FLOP", floppy_state },
    { "CHIP", chip_state },   { "SLOW", slow_state },
};
#define NBLOCKS (int)(sizeof kBlocks / sizeof kBlocks[0])
// Blocks a snapshot cannot do without (bits of kBlocks).
#define REQUIRED ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 12))

static bool put_u32(const amiga_stream_t *io, uint32_t v) {
    const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return io->write(io->ctx, b, 4);
}

bool amiga_state_save(const amiga_stream_t *io) {
    denise_sync();
    for (int i = 0; i < NBLOCKS; i++) {
        st_t s = { ST_SIZE, 0, true, io };
        kBlocks[i].fn(&s);
        if (!s.n) continue;                             // SLOW without slow RAM
        if (!io->write(io->ctx, kBlocks[i].id, 4) || !put_u32(io, s.n)) return false;
        s.mode = ST_SAVE;
        kBlocks[i].fn(&s);
        if (!s.ok) return false;
    }
    return true;
}

// The next block's header; false at the end (or a truncated header).
static bool next_block(const amiga_stream_t *io, char id[4], uint32_t *size) {
    uint8_t h[8];
    if (io->read(io->ctx, h, 8) != 8) return false;
    memcpy(id, h, 4);
    *size = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
    return true;
}

static bool skip(const amiga_stream_t *io, uint32_t n) {
    uint8_t b[64];
    while (n) {
        const uint32_t k = n < sizeof b ? n : (uint32_t)sizeof b;
        if (io->read(io->ctx, b, k) != k) return false;
        n -= k;
    }
    return true;
}

bool amiga_state_load(const amiga_stream_t *io) {
    char id[4];
    uint32_t size;
    // MACH first, checked before anything is touched.
    if (!next_block(io, id, &size) || memcmp(id, kBlocks[0].id, 4)) return false;
    st_t s = { ST_LOAD, size, true, io };
    mach_state(&s);
    if (!s.ok || !skip(io, s.n)) return false;

    amiga_reset(false);                                 // every module at a known start
    uint32_t seen = 1;
    while (next_block(io, id, &size)) {
        int b = 1;
        while (b < NBLOCKS && memcmp(id, kBlocks[b].id, 4)) b++;
        if (b == NBLOCKS) { if (!skip(io, size)) return false; continue; }
        st_t t = { ST_LOAD, size, true, io };
        kBlocks[b].fn(&t);
        if (!t.ok || !skip(io, t.n)) return false;
        seen |= 1u << b;
    }
    return (seen & REQUIRED) == REQUIRED;
}
