// Paula audio: four 8-bit DMA channels, period/volume, block interrupts.
//
// Channels advance in colour clocks and are point-sampled twice per line
// (AMIGA_AUDIO_RATE = 31300 Hz).  Channel 0+3 go left, 1+2 right.  Each
// channel follows the hardware's DMA state machine closely enough for the
// usual double-buffered replay routines: when a block is (re)started the
// location/length registers are latched and AUDx is raised, which is the
// software's cue to write the next block's AUDxLC/AUDxLEN.
//
// Not modelled yet: ADKCON attach (volume/period modulation), the audio
// low-pass filter (CIA-A PRA bit 1), and non-DMA "manual" playback beyond
// holding the last AUDxDAT value.
#include <string.h>
#include "amiga_int.h"

typedef struct {
    uint32_t lc, ptr;
    uint16_t len, cnt;       // words; 0 means 65536
    uint16_t per, vol, dat;
    int32_t  per_cnt;        // colour clocks left in the current sample
    bool     dma;            // DMA running
    bool     hi;             // next sample is the high byte of dat
    int8_t   sample;
} chan_t;

static chan_t ch[4];
static uint32_t audio_pos;

static inline bool dma_enabled(int i) {
    return (A.dmacon & DMAF_MASTER) && (A.dmacon & (DMAF_AUD0 << i));
}

static void block_start(chan_t *c, int i) {
    c->ptr = c->lc;
    c->cnt = c->len;
    amiga_set_intreq((uint16_t)(INTF_AUD0 << i));
}

static void fetch(chan_t *c, int i) {
    c->dat = chip_rw(c->ptr);
    c->ptr += 2;
    if (--c->cnt == 0) block_start(c, i);
}

void paula_dma_changed(void) {
    for (int i = 0; i < 4; i++) {
        chan_t *c = &ch[i];
        bool en = dma_enabled(i);
        if (en && !c->dma) {
            c->dma = true;
            block_start(c, i);
            fetch(c, i);
            c->hi = true;
            c->per_cnt = 0;
        } else if (!en && c->dma) {
            c->dma = false;
        }
    }
}

static void advance(chan_t *c, int i, int cc) {
    uint16_t per = c->per < 124 && c->dma ? 124 : c->per;
    if (!per) return;
    c->per_cnt -= cc;
    while (c->per_cnt <= 0) {
        c->per_cnt += per;
        if (c->hi) {
            c->sample = (int8_t)(c->dat >> 8);
            c->hi = false;
        } else {
            c->sample = (int8_t)c->dat;
            c->hi = true;
            if (c->dma) fetch(c, i);
        }
    }
}

void paula_half_line(void) {
    int32_t l = 0, r = 0;
    // 227 colour clocks per line: alternate 113 / 114.
    int cc = (audio_pos & 1) ? 114 : 113;
    for (int i = 0; i < 4; i++) {
        chan_t *c = &ch[i];
        if (c->dma || c->per) advance(c, i, cc);
        int v = c->vol > 64 ? 64 : c->vol;
        int32_t s = c->sample * v;              // -8192 .. 8128
        if (i == 0 || i == 3) l += s; else r += s;
    }
    if (A.cfg.audio_ring) {
        uint32_t idx = (audio_pos & (A.cfg.audio_frames - 1)) * 2;
        A.cfg.audio_ring[idx]     = (int16_t)(l * 2);
        A.cfg.audio_ring[idx + 1] = (int16_t)(r * 2);
    }
    audio_pos++;
}

AMIGA_HOT uint32_t paula_audio_pos(void) { return audio_pos; }

void paula_write(uint32_t reg, uint16_t v) {
    int i = (int)((reg - R_AUD0LCH) >> 4);
    if (i < 0 || i > 3) return;
    chan_t *c = &ch[i];
    switch (reg & 0xE) {
    case 0x0: c->lc = (c->lc & 0xFFFF) | ((uint32_t)(v & 7) << 16); break;
    case 0x2: c->lc = (c->lc & 0xFFFF0000) | (v & 0xFFFE); break;
    case 0x4: c->len = v; break;
    case 0x6: c->per = v; break;
    case 0x8: c->vol = v & 0x7F; break;
    case 0xA: c->dat = v; if (!c->dma) amiga_set_intreq((uint16_t)(INTF_AUD0 << i)); break;
    default: break;
    }
}

void paula_reset(void) {
    memset(ch, 0, sizeof ch);
}
