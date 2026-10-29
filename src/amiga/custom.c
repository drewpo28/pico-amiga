// Custom chip register file ($DFF000): routing to the chip modules, plus the
// registers that belong to nobody in particular (DMACON, INTENA/INTREQ,
// ADKCON, beam counters, joystick/pot ports, serial).
#include "amiga_int.h"
#include "m68k.h"

static uint16_t clxcon;

AMIGA_HOT static bool display_reg(uint32_t reg) {
    return (reg >= R_DIWSTRT && reg <= R_DDFSTOP) ||
           (reg >= R_BPL1PTH && reg < R_BPL1PTH + 24) ||
           (reg >= R_BPLCON0 && reg <= R_BPL2MOD) ||
           (reg >= R_SPR0PTH && reg < R_COLOR00 + 64) ||
           (reg == R_DIWHIGH && A.ecs);
}

AMIGA_HOT uint16_t custom_read(uint32_t reg) {
    switch (reg) {
    case R_DMACONR:
        return (uint16_t)((A.dmacon & 0x07FF) |
                          (blitter_busy() ? DMAF_BBUSY : 0) |
                          (blitter_zero() ? DMAF_BZERO : 0));
    case R_VPOSR:   // Agnus ID: 0 = OCS PAL, $20 = ECS PAL (8372A / 8375)
        return (uint16_t)((A.lof ? 0x8000 : 0) | (A.ecs ? 0x2000 : 0) | ((A.vpos >> 8) & 1));
    case R_VHPOSR:
        return (uint16_t)(((A.vpos & 0xFF) << 8) | amiga_hpos());
    case R_JOY0DAT: return A.joy0dat;
    case R_JOY1DAT: {
        uint8_t d = A.joy_dirs[1];
        int up = d & 1, down = (d >> 1) & 1, left = (d >> 2) & 1, right = (d >> 3) & 1;
        if (A.joy1dat && !d) return A.joy1dat;
        return (uint16_t)((right << 1) | (right ^ down) | (left << 9) | ((left ^ up) << 8));
    }
    case R_CLXDAT:  return 0x8000;
    case R_ADKCONR: return A.adkcon;
    case R_POT0DAT: case R_POT1DAT: return 0;
    case R_POTGOR:
        // Pot pins read high unless driven; the right mouse button pulls DATLY low.
        return (uint16_t)(0xFF00 & ~(A.mouse_rmb ? 0x0400 : 0));
    case R_SERDATR: return 0x3800;                       // TBE | TSRE | RXD idle
    case R_DSKBYTR: return floppy_read(reg);
    case R_INTENAR: return A.intena;
    case R_INTREQR: return A.intreq;
    case R_DENISEID: return A.ecs ? 0xFFFC : 0xFFFF;     // ECS 8373: $FC; OCS has no ID register
    // A read strobes too: Zynaps starts its copper list with move.w $dff088,d0,
    // and the OS list's last MOVE (BPLCON0 = 0 planes) must not run after it.
    case R_COPJMP1: case R_COPJMP2: copper_write(reg, 0); return 0;
    default:        return 0;
    }
}

AMIGA_HOT void custom_write(uint32_t reg, uint16_t v, int hpos, bool from_copper) {
    (void)from_copper;
    if (display_reg(reg)) { denise_write(reg, v, hpos); return; }
    if (reg >= R_BLTCON0 && reg <= R_BLTADAT) { blitter_write(reg, v); return; }
    if (reg >= R_AUD0LCH && reg < R_AUD0LCH + 0x40) { paula_write(reg, v); return; }
    switch (reg) {
    case R_DMACON: {
        uint16_t old = A.dmacon;
        if (v & 0x8000) A.dmacon |= v & 0x07FF; else A.dmacon &= ~(v & 0x07FF);
        if (old != A.dmacon) {
            blitter_dma_changed();
            paula_dma_changed();
            copper_dma_changed();
        }
        break;
    }
    case R_INTENA:
        if (v & 0x8000) A.intena |= v & 0x7FFF; else A.intena &= ~(v & 0x7FFF);
        amiga_update_ipl();
        break;
    case R_INTREQ:
        if (v & 0x8000) A.intreq |= v & 0x7FFF; else A.intreq &= ~(v & 0x7FFF);
        amiga_update_ipl();
        break;
    case R_ADKCON:
        if (v & 0x8000) A.adkcon |= v & 0x7FFF; else A.adkcon &= ~(v & 0x7FFF);
        break;
    case R_COPCON: A.copcon = v; break;
    case R_COP1LCH: case R_COP1LCL: case R_COP2LCH: case R_COP2LCL:
    case R_COPJMP1: case R_COPJMP2:
        copper_write(reg, v);
        break;
    case R_DSKPTH: case R_DSKPTL: case R_DSKLEN: case R_DSKSYNC:
        floppy_write(reg, v);
        break;
    case R_SERDAT:
        if (A.cfg.serial_out) A.cfg.serial_out(A.cfg.ctx, (uint8_t)v);
        amiga_set_intreq(INTF_TBE);                      // the "transmission" is instant
        break;
    case R_SERPER: A.serper = v; break;
    case R_POTGO:  A.potgo = v; break;
    case R_JOYTEST:
        A.joy0dat = (uint16_t)((A.joy0dat & 0x0303) | (v & 0xFCFC));
        A.joy1dat = (uint16_t)((A.joy1dat & 0x0303) | (v & 0xFCFC));
        break;
    case R_CLXCON: clxcon = v; break;
    case R_BEAMCON0: if (A.ecs) A.beamcon0 = v; break;
    default: break;   // VPOSW, VHPOSW, REFPTR, STRxxx, BPLxDAT, DSKDAT, COPINS
    }
}

void custom_reset(void) {
    A.dmacon = 0; A.intena = 0; A.intreq = 0; A.adkcon = 0;
    A.copcon = 0; A.potgo = 0; A.serper = 0;
    A.beamcon0 = 0x0020;                                 // PAL
    clxcon = 0;
    amiga_update_ipl();
}

AMIGA_HOT void amiga_set_intreq(uint16_t bits) {
    if ((A.intreq & bits) == bits) return;
    A.intreq |= bits;
    amiga_update_ipl();
}

AMIGA_HOT void amiga_update_ipl(void) {
    uint16_t act = A.intena & A.intreq & 0x3FFF;
    int level = 0;
    if ((A.intena & INTF_INTEN) && act) {
        if      (act & 0x2000) level = 6;
        else if (act & 0x1800) level = 5;
        else if (act & 0x0780) level = 4;
        else if (act & 0x0070) level = 3;
        else if (act & 0x0008) level = 2;
        else                   level = 1;
    }
    m68k_set_irq((unsigned)level);
    // Interrupts are only sampled when m68k_execute() is entered: cut the slice
    // short so a newly raised level is taken after the current instruction.
    if (A.in_cpu && level) m68k_end_timeslice();
}
