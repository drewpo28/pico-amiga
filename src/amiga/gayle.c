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
// No drive is fitted yet and no card is in the slot.  WinUAE's "IDE killer"
// answers an empty port: the status register $7F, every other register $FF, so
// scsi.device gives up at once instead of waiting ~30 s for a drive to spin up.
#include "amiga_int.h"

static uint8_t cs, cs_mask, irq, inten, cfg;
static uint8_t id_cnt;

void gayle_reset(void) {
    cs = cs_mask = irq = inten = cfg = 0;
    id_cnt = 0;
}

uint8_t gayle_read(uint32_t a) {
    if ((a & 0xA0000) != 0xA0000) return 0;
    a &= 0xFFFF;
    switch (a) {
    case 0x8000: return (uint8_t)(cs_mask | cs);
    case 0x9000: return irq;
    case 0xA000: return inten;
    case 0xB000: return cfg & 0x0F;
    default: break;
    }
    if (a >= 0x4000) return 0;
    return ((a & ~0x2020u) >> 2) == 7 ? 0x7F : 0xFF;     // IDE: no drive
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
    default: break;                         // IDE: no drive takes it
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
}
