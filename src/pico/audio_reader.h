// Reading the Paula ring at the output rate (audio_out.c's tick, the PC host's
// SDL callback).  The emulator fills the ring a frame at a time, paced by the
// display, so its rate only averages to the output's: the reader keeps the
// ring's fill near AU_TARGET by reading a little faster or slower (RetroArch's
// dynamic rate control), between -10% and +5%, with linear interpolation.  An
// emulation running slower than real time then plays lower instead of
// stopping the sound - holding the last frame (a dip, "the volume taken away
// and back") is left for an empty ring.  The fill is low-passed over ~6
// frames: it saws by a frame's worth at every frame, and following that would
// warble.
#pragma once
#include <stdint.h>
#include "amiga.h"

#define AU_TARGET   (AMIGA_AUDIO_RATE / 50 * 2)       // frames in the ring: 40 ms
#define AU_GAIN     8                                 // step / 65536 per frame of error: 5% at ~410
#define AU_STEP_MIN 58982                             // 0.90
#define AU_STEP_MAX 68813                             // 1.05

typedef struct {
    uint32_t rd, frac;          // read position, its fraction (Q16)
    int32_t  fill;              // low-passed fill, Q12
    int16_t  l, r;              // the last output
    uint32_t held, skipped;     // ticks on an empty ring, frames jumped over
} au_reader_t;

static inline void au_reader_start(au_reader_t *a, uint32_t write_pos) {
    a->rd = write_pos - AU_TARGET;
    a->frac = 0;
    a->fill = AU_TARGET << 12;
    a->l = a->r = 0;
}

// One output frame from `ring` (frames, a power of two) written up to `wp`.
static inline void au_reader_next(au_reader_t *a, const int16_t *ring, uint32_t frames, uint32_t wp) {
    uint32_t lag = wp - a->rd;
    if (lag > frames / 2) {                           // lost track (a reset, a long stall)
        a->skipped += lag;
        a->rd = wp - AU_TARGET;
        a->frac = 0;
        a->fill = AU_TARGET << 12;
        lag = AU_TARGET;
    }
    a->fill += (((int32_t)lag << 12) - a->fill) >> 12;
    int32_t step = 65536 + ((a->fill >> 12) - (int32_t)AU_TARGET) * AU_GAIN;
    if (step < AU_STEP_MIN) step = AU_STEP_MIN;
    if (step > AU_STEP_MAX) step = AU_STEP_MAX;
    if (lag < 2) { a->held++; return; }               // nothing to play: hold
    const uint32_t m = frames - 1;
    const int16_t *p0 = ring + ((a->rd & m) * 2), *p1 = ring + (((a->rd + 1) & m) * 2);
    const int32_t f = (int32_t)(a->frac >> 1);       // Q15: the product stays within 32 bits
    a->l = (int16_t)(p0[0] + (((p1[0] - p0[0]) * f) >> 15));
    a->r = (int16_t)(p0[1] + (((p1[1] - p0[1]) * f) >> 15));
    a->frac += (uint32_t)step;
    a->rd += a->frac >> 16;
    a->frac &= 0xFFFF;
}
