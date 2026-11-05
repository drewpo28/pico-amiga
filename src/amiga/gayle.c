// Gayle: the A600's (and A1200's) gate array - IDE port, PCMCIA slot, its ID.
// Taken from WinUAE's gayle.cpp (IDE_A600A1200 with the PCMCIA slot fitted):
//
//   $D80000-$DDFFFF  Gayle's bank; only addresses with A19 and A17 set answer
//                    ($DA0000 and its mirror $DB0000), the rest read 0
//     +$0000-$3FFF   IDE task file: register (offset & ~$2020) >> 2, bit $400
//                    of it the secondary (control) block
//     +$8000         PCMCIA status (CS)    +$9000  interrupt request (IRQ)
//     +$A000         interrupt enable (INT) +$B000  PCMCIA config (CFG)
//   $DD0000-$DEFFFF  the ID: each byte read of offset $1000 shifts out one bit
//                    of it in bit 7, MSB first ($D0: 1,1,0,1,0,0,0,0); any
//                    write there starts it over.  AGA's Gayle reads $D1.
//
// The IDE port has the drive of ide.c (an HDF) or none; no card is in the slot.
// With no drive, WinUAE's "IDE killer" answers: the status register $7F, every
// other register $FF, so scsi.device gives up at once instead of waiting ~30 s
// for a drive to spin up.  The drive's INTRQ sets IRQ bit 7 (and reads in CS
// bit 7); IRQ & INT bits 7 or 3 raise level 2 (INTREQ PORTS), as long as they
// are set: the handler clears IRQ by writing it with the bit 0.
#include "amiga_int.h"

static uint8_t cs, cs_mask, irq, inten, cfg;
static uint8_t id_cnt;

void gayle_reset(void) {
    cs = cs_mask = irq = inten = cfg = 0;
    id_cnt = 0;
    ide_reset();
}

uint8_t gayle_read(uint32_t a) {
    if ((a & 0xA0000) != 0xA0000) return 0;
    a &= 0xFFFF;
    switch (a) {
    case 0x8000: return (uint8_t)(cs_mask | cs | (ide_irq() ? 0x80 : 0));
    case 0x9000: return irq;
    case 0xA000: return inten;
    case 0xB000: return cfg & 0x0F;
    default: break;
    }
    if (a >= 0x4000) return 0;
    return ide_read((int)((a & ~0x2020u) >> 2));       // the data register reads 0 as a byte
}

// Words: the IDE data register moves a word; the rest are two byte accesses.
static bool is_data(uint32_t a) {
    return (a & 0xA0000) == 0xA0000 && (a & 0xFFFF) < 0x4000 && ((a & 0xFFFF & ~0x2020u) >> 2) == 0;
}
uint16_t gayle_read16(uint32_t a) {
    if (is_data(a)) return ide_read_data();
    return (uint16_t)(gayle_read(a) << 8 | gayle_read(a + 1));
}
void gayle_write16(uint32_t a, uint16_t v) {
    if (is_data(a)) { ide_write_data(v); return; }
    gayle_write(a, (uint8_t)(v >> 8));
    gayle_write(a + 1, (uint8_t)v);
}

// Once a line (Gayle machines): the drive's interrupt into IRQ, IRQ & INT to Paula.
void gayle_line(void) {
    if (ide_line()) irq |= 0x80;
    if (inten & irq & 0x88) amiga_set_intreq(INTF_PORTS);
}

void gayle_write(uint32_t a, uint8_t v) {
    if ((a & 0xA0000) != 0xA0000) return;
    switch (a & 0xFFFF) {
    case 0x8000:                            // the card-detect bits are the slot's
        cs_mask = v & (uint8_t)~3u;
        cs = (uint8_t)((cs & ~3u) | (v & 3u));
        break;
    case 0x9000: irq = (uint8_t)((irq & v) | (v & 3u)); break;   // 0 clears a flag
    case 0xA000: inten = v; break;
    case 0xB000: cfg = v; break;
    default:
        if ((a & 0xFFFF) < 0x4000) ide_write((int)(((a & 0xFFFF) & ~0x2020u) >> 2), v);
        break;
    }
}

uint8_t gayle_id_read(uint32_t a) {
    if ((a & 0xFFFF) != 0x1000) return 0;
    const uint8_t id = A.cfg.gayle;
    const uint8_t v = id_cnt < 8 && (id & (0x80u >> id_cnt)) ? 0x80 : 0x00;
    if (id_cnt < 8) id_cnt++;
    return v;
}

void gayle_id_write(void) { id_cnt = 0; }

void gayle_state(st_t *s) {
    st_u8(s, &cs); st_u8(s, &cs_mask); st_u8(s, &irq); st_u8(s, &inten); st_u8(s, &cfg);
    st_u8(s, &id_cnt);
    ide_state(s);
}
