// The two 8520 CIAs.
//
// Timers run off the E clock (CPU clock / 10 = 709 kHz) and are synced lazily:
// every register access and every HSYNC catches the chip up to "now", so a
// program polling a timer reads the exact count, while interrupts are raised
// at worst one line late.
//
// Wiring on an A500:
//   CIA-A ($BFE001, odd)  PRA: /FIR1 /FIR0 /RDY /TK0 /WPRO /CHNG /LED OVL
//                         PRB: parallel data; SP/CNT: keyboard; TOD: VSYNC
//                         /IRQ -> INT2 (PORTS)
//   CIA-B ($BFD000, even) PRA: serial/parallel control (unused here)
//                         PRB: /MTR /SEL3 /SEL2 /SEL1 /SEL0 /SIDE DIR /STEP
//                         /FLAG: disk index; TOD: HSYNC; /IRQ -> INT6 (EXTER)
#include <string.h>
#include "amiga_int.h"

enum { PRA, PRB, DDRA, DDRB, TALO, TAHI, TBLO, TBHI, TODLO, TODMID, TODHI, UNUSED, SDR, ICR, CRA, CRB };

typedef struct {
    uint8_t  pra, prb, ddra, ddrb;
    uint16_t ta, tb, ta_latch, tb_latch;
    uint8_t  cra, crb;
    uint32_t tod, tod_latch, alarm;
    bool     tod_latched, tod_stopped;
    uint8_t  sdr, icr, imask;
    uint64_t last_e;
} cia_t;

static cia_t cia[2];
static bool  kbd_ack;      // CPU pulsed SPMODE (keyboard handshake)

static uint64_t e_now(void) { return amiga_now() / 10; }

static void cia_irq(int w) {
    cia_t *c = &cia[w];
    if (c->icr & c->imask & 0x1F)
        amiga_set_intreq(w ? INTF_EXTER : INTF_PORTS);
}

static void ta_underflow(int w);

static void tb_tick(int w, uint32_t n) {
    cia_t *c = &cia[w];
    while (n) {
        if (n <= c->tb) { c->tb -= (uint16_t)n; return; }
        n -= (uint32_t)c->tb + 1;
        c->icr |= 0x02;
        c->tb = c->tb_latch;
        if (c->crb & 0x08) { c->crb &= ~1; return; }   // one-shot
    }
}

static void ta_underflow(int w) {
    cia_t *c = &cia[w];
    c->icr |= 0x01;
    c->ta = c->ta_latch;
    if (c->cra & 0x08) c->cra &= ~1;
    if ((c->crb & 0x01) && (c->crb & 0x60) == 0x40) tb_tick(w, 1);   // TB counts TA underflows
}

static void cia_sync(int w) {
    cia_t *c = &cia[w];
    uint64_t now = e_now();
    uint32_t n = (uint32_t)(now - c->last_e);
    c->last_e = now;
    if (!n) return;
    if ((c->cra & 0x21) == 0x01) {                    // running, counting E clocks
        uint32_t m = n;
        while (m && (c->cra & 1)) {
            if (m <= c->ta) { c->ta -= (uint16_t)m; break; }
            m -= (uint32_t)c->ta + 1;
            ta_underflow(w);
        }
    }
    if ((c->crb & 0x61) == 0x01) tb_tick(w, n);       // running, counting E clocks
}

static void tod_tick(int w) {
    cia_t *c = &cia[w];
    if (c->tod_stopped) return;
    c->tod = (c->tod + 1) & 0xFFFFFF;
    if (c->tod == c->alarm) { c->icr |= 0x04; cia_irq(w); }
}

void cia_reset(void) {
    memset(cia, 0, sizeof cia);
    for (int w = 0; w < 2; w++) {
        cia[w].ta = cia[w].tb = cia[w].ta_latch = cia[w].tb_latch = 0xFFFF;
        cia[w].last_e = e_now();
    }
    kbd_ack = false;
    mem_set_overlay(true);          // DDRA = 0: OVL floats high
    floppy_cia_prb(0xFF);
}

static uint8_t pra_inputs(int w) {
    if (w) return 0xFF;
    uint8_t v = 0xFF;
    if (A.joy_fire[1]) v &= ~0x80;
    if (A.mouse_lmb || A.joy_fire[0]) v &= ~0x40;
    v = (uint8_t)((v & ~0x3C) | (floppy_cia_pra() & 0x3C));
    return v;
}

uint8_t cia_read(int w, int reg) {
    cia_t *c = &cia[w];
    cia_sync(w);
    switch (reg) {
    case PRA:  return (uint8_t)((c->pra & c->ddra) | (pra_inputs(w) & ~c->ddra));
    case PRB:  return (uint8_t)((c->prb & c->ddrb) | (0xFF & ~c->ddrb));
    case DDRA: return c->ddra;
    case DDRB: return c->ddrb;
    case TALO: return (uint8_t)c->ta;
    case TAHI: return (uint8_t)(c->ta >> 8);
    case TBLO: return (uint8_t)c->tb;
    case TBHI: return (uint8_t)(c->tb >> 8);
    case TODLO: {
        uint32_t t = c->tod_latched ? c->tod_latch : c->tod;
        c->tod_latched = false;
        return (uint8_t)t;
    }
    case TODMID: return (uint8_t)((c->tod_latched ? c->tod_latch : c->tod) >> 8);
    case TODHI:
        c->tod_latch = c->tod; c->tod_latched = true;
        return (uint8_t)(c->tod >> 16);
    case SDR:  return c->sdr;
    case ICR: {
        uint8_t v = c->icr;
        if (c->icr & c->imask & 0x1F) v |= 0x80;
        c->icr = 0;
        return v;
    }
    case CRA:  return c->cra;
    case CRB:  return c->crb;
    default:   return 0xFF;
    }
}

static void port_outputs(int w) {
    cia_t *c = &cia[w];
    if (w == 0) {
        bool ovl = (c->ddra & 1) ? (c->pra & 1) : true;
        mem_set_overlay(ovl);
    } else {
        floppy_cia_prb((uint8_t)((c->prb & c->ddrb) | (0xFF & ~c->ddrb)));
    }
}

void cia_write(int w, int reg, uint8_t v) {
    cia_t *c = &cia[w];
    cia_sync(w);
    switch (reg) {
    case PRA:  c->pra = v;  port_outputs(w); break;
    case PRB:  c->prb = v;  port_outputs(w); break;
    case DDRA: c->ddra = v; port_outputs(w); break;
    case DDRB: c->ddrb = v; port_outputs(w); break;
    case TALO: c->ta_latch = (uint16_t)((c->ta_latch & 0xFF00) | v); break;
    case TAHI:
        c->ta_latch = (uint16_t)((c->ta_latch & 0x00FF) | (v << 8));
        if (!(c->cra & 1)) c->ta = c->ta_latch;
        if (c->cra & 0x08) { c->ta = c->ta_latch; c->cra |= 1; }   // one-shot: load + start
        break;
    case TBLO: c->tb_latch = (uint16_t)((c->tb_latch & 0xFF00) | v); break;
    case TBHI:
        c->tb_latch = (uint16_t)((c->tb_latch & 0x00FF) | (v << 8));
        if (!(c->crb & 1)) c->tb = c->tb_latch;
        if (c->crb & 0x08) { c->tb = c->tb_latch; c->crb |= 1; }
        break;
    case TODLO:
        if (c->crb & 0x80) c->alarm = (c->alarm & 0xFFFF00) | v;
        else { c->tod = (c->tod & 0xFFFF00) | v; c->tod_stopped = false; }
        break;
    case TODMID:
        if (c->crb & 0x80) c->alarm = (c->alarm & 0xFF00FF) | ((uint32_t)v << 8);
        else c->tod = (c->tod & 0xFF00FF) | ((uint32_t)v << 8);
        break;
    case TODHI:
        if (c->crb & 0x80) c->alarm = (c->alarm & 0x00FFFF) | ((uint32_t)v << 16);
        else { c->tod = (c->tod & 0x00FFFF) | ((uint32_t)v << 16); c->tod_stopped = true; }
        break;
    case SDR:  c->sdr = v; break;
    case ICR:
        if (v & 0x80) c->imask |= v & 0x1F; else c->imask &= ~(v & 0x1F);
        cia_irq(w);
        break;
    case CRA:
        if (w == 0 && (v & 0x40) && !(c->cra & 0x40)) kbd_ack = true;   // SPMODE -> output
        if (v & 0x10) c->ta = c->ta_latch;                               // force load
        c->cra = v & ~0x10;
        break;
    case CRB:
        if (v & 0x10) c->tb = c->tb_latch;
        c->crb = v & ~0x10;
        break;
    default: break;
    }
}

void cia_line(void) {
    cia_sync(0); cia_sync(1);
    tod_tick(1);                    // CIA-B TOD counts HSYNC
    cia_irq(0); cia_irq(1);
}

void cia_vsync(void) { tod_tick(0); }

void cia_flag(int w) { cia[w].icr |= 0x10; cia_irq(w); }

void cia_keyboard_byte(uint8_t sdr) {
    cia_sync(0);
    cia[0].sdr = sdr;
    cia[0].icr |= 0x08;
    kbd_ack = false;
    cia_irq(0);
}

bool cia_keyboard_handshake(void) { return kbd_ack; }
