// DVI/HDMI output through a PIO TMDS serializer, for boards whose HDMI pins are
// not the HSTX ones (PICO_DV: clock on GPIO 6/7, data on 8-13).
//
// Same mode as video_hstx.c - 720x576 @ 50.06 Hz, 25.2 MHz pixel, 832 x 605 -
// so the PIO runs at 252 MHz: clk_sys / 1.5 at 378, / 2 at 504 (pico-speccy
// runs its PIO HDMI from the same half-integer dividers).
//
// Every output pixel is two 32-bit words of pin patterns (tmds_pio.h), i.e.
// 1664 words per line and ~200 MB/s of DMA.  Core 1's main loop TMDS-encodes
// up to two output lines ahead of the beam (66 us) from lookup tables of
// balanced character pairs - doubled lowres pixels, or hires pairs of two
// different colours; the second line of a row that is not interlaced sends the
// first one's buffer again.  One line ahead (33 us) was not enough for a full
// hires encode at 504 MHz: late lines went out with the pixels of two lines
// earlier and hires text shook.  The DMA
// ping-pong + IRQ structure is the one in video_hstx.c, each line being two
// transfers: blanking (fp + sync + bp) and the active part.
#include "video.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/structs/bus_ctrl.h"
#include "amiga.h"
#include "tmds_pio.h"
#include "tmds.pio.h"
#include "hdmi_audio.h"
#include "board.h"

#ifndef HDMI_BASE_PIN
#define HDMI_BASE_PIN 6
#endif
#define PIN_CLK   HDMI_BASE_PIN
#define PIN_DATA  (HDMI_BASE_PIN + 2)

#define H_FP       12
#define H_SYNC     64
#define H_BP       36
#define H_ACTIVE   720
#define H_BLANK    (H_FP + H_SYNC + H_BP)
#define V_FP       5
#define V_SYNC     5
#define V_BP       19
#define V_ACTIVE   576
#define V_TOTAL    (V_FP + V_SYNC + V_BP + V_ACTIVE)

#define VPIO       pio0
#define VSM        0
#define DMACH_PING 0
#define DMACH_PONG 1

static const amiga_row_t *fb;
static uint32_t tab_r[16][4];
static uint32_t tab_gb[256][4];
static uint32_t tab_mix[3][16][16][4];             // hires pairs of different colours
static uint32_t blank_line[2][H_BLANK * 2];        // [vsync active?]
static uint32_t blank_active[2][H_ACTIVE * 2];     // the active part of a vblank line
static uint32_t linebuf[4][AMIGA_FB_W * 4];       // by output line & 3
static const uint32_t *volatile line_src[4];      // what output line n sends: [n & 3]
static volatile int      line_ready[4] = { -1, -1, -1, -1 };   // output line in [n & 3]
static volatile uint32_t late;                    // lines sent before they were encoded
// The blanking part of each line, by line parity: blank_line plus the line's data
// island in the sync pulse and, before an active line, the video preamble/guard band.
static uint32_t blankbuf[2][H_BLANK * 2];
static uint32_t terc_w[3][16][2];                  // TERC4 nibble -> pin words, per lane
static uint32_t isl_pre_w[2][10][2], isl_post_w[2][2][2];   // [vsync]
static uint32_t lead_w[HDMI_VLEAD_PX][2];
// The packets that never change, encoded once: [HDMI_PK_*][vsync][character].
static uint32_t isl_fixed_w[HDMI_PK_FIXED][2][32][2];
// What blankbuf[parity] holds besides the packet: vsync | active << 1 (0xFF: nothing
// yet).  Only the 32 packet characters change from line to line of one kind.
static uint8_t  blank_kind[2] = { 0xFF, 0xFF };
// Core 1's time (DWT cycles): the line IRQ and the TMDS encoding (video.h).
static volatile uint32_t irq_cyc, enc_cyc;
// Line IRQs that finished after the other channel had already run out: the
// chain then restarted this one with its spent settings (a torn line or island).
static volatile uint32_t irq_missed;

static volatile uint32_t frames;
static volatile int      shown_line = -1;          // output line whose pixels were last posted
static bool     dma_pong, part_b;
static uint32_t v_line;

static void fill_ctrl(uint32_t *dst, int px, int vs, int hs) {
    uint32_t w[2];
    tmds_ctrl_words(vs, hs, w);
    for (int i = 0; i < px; i++) { dst[2 * i] = w[0]; dst[2 * i + 1] = w[1]; }
}

// One character (three symbols packed as hdmi_audio.h does) as two pin words.
static void char_words(uint32_t c, uint32_t w[2]) {
    w[0] = w[1] = 0;
    for (int lane = 0; lane < 3; lane++) {
        uint32_t lw[2];
        tmds_lane_words(lane, (uint16_t)((c >> (10 * lane)) & 0x3FF), lw);
        w[0] |= lw[0];
        w[1] |= lw[1];
    }
}

static void build_static(void) {
    tmds_build_tables(tab_r, tab_gb);
    for (int lane = 0; lane < 3; lane++)
        for (int n = 0; n < 16; n++) tmds_lane_words(lane, (uint16_t)hdmi_terc4(n), terc_w[lane][n]);
    for (int vs = 0; vs < 2; vs++) {
        uint32_t pre[10], post[2];
        hdmi_island_frame(pre, post, vs);
        for (int i = 0; i < 10; i++) char_words(pre[i], isl_pre_w[vs][i]);
        for (int i = 0; i < 2; i++) char_words(post[i], isl_post_w[vs][i]);
    }
    for (int i = 0; i < HDMI_VLEAD_PX; i++) char_words(hdmi_video_lead[i], lead_w[i]);
    for (int id = 0; id < HDMI_PK_FIXED; id++)
        for (int vs = 0; vs < 2; vs++) {
            uint8_t nib[32][3];
            hdmi_audio_fixed_nibbles(id, vs, nib);
            for (int t = 0; t < 32; t++)
                for (int k = 0; k < 2; k++)
                    isl_fixed_w[id][vs][t][k] = terc_w[0][nib[t][0]][k] | terc_w[1][nib[t][1]][k] | terc_w[2][nib[t][2]][k];
        }
    tmds_build_mixed(tab_mix);
    for (int vs = 0; vs < 2; vs++) {
        int vlev = vs ? 0 : 1;                         // negative polarity: active = 0
        fill_ctrl(blank_line[vs], H_FP, vlev, 1);
        fill_ctrl(blank_line[vs] + 2 * H_FP, H_SYNC, vlev, 0);
        fill_ctrl(blank_line[vs] + 2 * (H_FP + H_SYNC), H_BP, vlev, 1);
        fill_ctrl(blank_active[vs], H_ACTIVE, vlev, 1);
    }
}

static void __not_in_flash_func(dma_irq)(void) {
    const uint32_t t0 = board_cycles();
    uint ch = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *c = &dma_hw->ch[ch];
    dma_hw->intr = 1u << ch;
    dma_pong = !dma_pong;

    const bool vsync = v_line >= V_FP && v_line < V_FP + V_SYNC;
    const bool active = v_line >= V_TOTAL - V_ACTIVE;
    if (!part_b) {
        // Line v_line's blanking: posted while the other channel streams the
        // previous line's 720 active pixels, which is the time it has to be built.
        uint32_t *d = blankbuf[v_line & 1];
        const uint8_t kind = (uint8_t)(vsync | active << 1);
        uint32_t *isl = d + 2 * H_FP;
        if (blank_kind[v_line & 1] != kind) {
            // A new kind of line for this buffer: the sync levels, the island's
            // frame and the video lead.  A volatile copy: GCC would turn a plain
            // loop into memcpy, which is in flash.
            blank_kind[v_line & 1] = kind;
            const uint32_t *t = blank_line[vsync];
            for (volatile uint32_t *v = d; v < d + H_BLANK * 2; ) *v++ = *t++;
            for (int i = 0; i < 10; i++) { isl[2 * i] = isl_pre_w[vsync][i][0]; isl[2 * i + 1] = isl_pre_w[vsync][i][1]; }
            uint32_t *post = isl + 2 * 42;
            for (int i = 0; i < 2; i++) { post[2 * i] = isl_post_w[vsync][i][0]; post[2 * i + 1] = isl_post_w[vsync][i][1]; }
            if (active) {
                uint32_t *lead = d + 2 * (H_BLANK - HDMI_VLEAD_PX);
                for (int i = 0; i < HDMI_VLEAD_PX; i++) { lead[2 * i] = lead_w[i][0]; lead[2 * i + 1] = lead_w[i][1]; }
            }
        }
        // The 32 packet characters.
        uint32_t *pk = isl + 2 * 10;
        uint8_t nib[32][3];
        const int id = hdmi_audio_packet(nib, active ? 0 : v_line + 1, vsync);
        if (id >= 0) {
            const uint32_t *s = &isl_fixed_w[id][vsync][0][0];
            for (int i = 0; i < 64; i++) pk[i] = s[i];
        } else {
            for (int t = 0; t < 32; t++) {
                pk[2 * t]     = terc_w[0][nib[t][0]][0] | terc_w[1][nib[t][1]][0] | terc_w[2][nib[t][2]][0];
                pk[2 * t + 1] = terc_w[0][nib[t][0]][1] | terc_w[1][nib[t][1]][1] | terc_w[2][nib[t][2]][1];
            }
        }
        c->read_addr = (uintptr_t)d;
        c->transfer_count = H_BLANK * 2;
        part_b = true;
    } else {
        if (active) {
            int out = (int)(v_line - (V_TOTAL - V_ACTIVE));
            if (line_ready[out & 3] != out) late++;
            line_ready[out & 3] = -1;
            c->read_addr = (uintptr_t)line_src[out & 3];
            shown_line = out;
        } else {
            c->read_addr = (uintptr_t)blank_active[vsync];
        }
        c->transfer_count = H_ACTIVE * 2;
        part_b = false;
        if (++v_line == V_TOTAL) { v_line = 0; frames++; shown_line = -1; }
    }
    if (!(dma_hw->ch[ch == DMACH_PING ? DMACH_PONG : DMACH_PING].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS))
        irq_missed++;
    irq_cyc += board_cycles() - t0;
}

// Encode output line `out` (0..575) into linebuf[out & 3].
// Hires and interlaced rows go pair by pair, straight from the row's indices; a
// pair equal to the one before it (backgrounds, text) only stores the same four
// words again.  A full pair encode is ~50 cycles, and 360 of them plus the DMA
// IRQs did not fit the 66 us of a row at 504 MHz.
static void __not_in_flash_func(encode_line)(int out) {
    const uint32_t t0 = board_cycles();
    static bool prev_lace;                         // the row of line out - 1 was LACE4
    const amiga_row_t *row = &fb[out >> 1];
    const int sub = out & 1;
    uint32_t *dst = linebuf[out & 3];
    // The second line of a row that was not interlaced when its first line was
    // encoded: the same pixels, from the same buffer.
    if (sub && !prev_lace) {
        line_src[out & 3] = linebuf[(out - 1) & 3];
        line_ready[out & 3] = out;
        enc_cyc += board_cycles() - t0;
        return;
    }
    prev_lace = row->mode == AMIGA_ROW_LACE4;
    switch (row->mode) {
    case AMIGA_ROW_IDX8: {
        const uint16_t *pal = row->pal;
        typedef uint16_t __attribute__((may_alias)) u16a;
        const u16a *ix = (const u16a *)(const void *)row->px.idx;   // index pairs, left in the low byte
        uint32_t key = 0xFFFFFFFFu, w[4] = { 0, 0, 0, 0 };
        for (int x = 0; x < AMIGA_FB_W; x++, dst += 4) {
            const uint32_t k = ix[x];
            if (k != key) {
                key = k;
                tmds_pair(w, pal[k & 63], pal[(k >> 8) & 63], tab_r, tab_gb, tab_mix);
            }
            dst[0] = w[0]; dst[1] = w[1]; dst[2] = w[2]; dst[3] = w[3];
        }
        dst -= 4 * AMIGA_FB_W;
        break;
    }
    case AMIGA_ROW_LACE4: {
        const uint16_t *pal = row->pal + 16 * sub;
        const uint8_t *n = row->px.nib[sub];
        uint32_t key = 0xFFFFFFFFu, w[4] = { 0, 0, 0, 0 };
        for (int x = 0; x < AMIGA_FB_W; x++, dst += 4) {
            const uint32_t k = n[x];
            if (k != key) {
                key = k;
                tmds_pair(w, pal[k >> 4], pal[k & 15], tab_r, tab_gb, tab_mix);
            }
            dst[0] = w[0]; dst[1] = w[1]; dst[2] = w[2]; dst[3] = w[3];
        }
        dst -= 4 * AMIGA_FB_W;
        break;
    }
    default: {
        // A colour equal to the one before it (backgrounds) stores the same words.
        const uint16_t *src = row->px.rgb;
        uint32_t key = 0xFFFFFFFFu, w0 = 0, w1 = 0, w2 = 0, w3 = 0;
        uint32_t *q = dst;
        for (int x = 0; x < AMIGA_FB_W; x++, q += 4) {
            const uint32_t c = src[x];
            if (c != key) {
                key = c;
                const uint32_t *r = tab_r[(c >> 8) & 15], *gb = tab_gb[c & 0xFF];
                w0 = r[0] | gb[0]; w1 = r[1] | gb[1]; w2 = r[2] | gb[2]; w3 = r[3] | gb[3];
            }
            q[0] = w0; q[1] = w1; q[2] = w2; q[3] = w3;
        }
        break;
    }
    }
    line_src[out & 3] = dst;
    line_ready[out & 3] = out;
    enc_cyc += board_cycles() - t0;
}

// Core 1, forever: keep up to two output lines ahead of the beam (lines 0 and 1
// during the vertical blank), and render the emulator's lines in between.  Buffer n & 3 is free again once line n - 2 has
// been sent, i.e. while line n - 1 is on the wire.
static void __no_inline_not_in_flash_func(encode_loop)(void) {
    int done = 0;                                  // hdmi_start encoded line 0
    for (;;) {
        const int shown = shown_line;
        if (shown + 2 < done) done = -1;           // a new frame began
        int target = shown + 2;
        if (target >= 2 * AMIGA_FB_H) target = 2 * AMIGA_FB_H - 1;
        if (done < target) encode_line(++done);
        else               amiga_render_pump();   // Denise's lines, in small steps (amiga.h)
    }
}

void hdmi_start(const amiga_row_t *framebuffer) {
    fb = framebuffer;
    hdmi_audio_init(25200000u, H_BLANK + H_ACTIVE, V_TOTAL);
    build_static();
    encode_line(0);

#if HDMI_BASE_PIN >= 32
    pio_set_gpio_base(VPIO, 16);     // ZERO2: display on GPIO 32-39
#endif
    uint offset = pio_add_program(VPIO, &tmds_program);
    pio_sm_config c = tmds_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, PIN_CLK);
    sm_config_set_out_pins(&c, PIN_DATA, 6);
    sm_config_set_out_shift(&c, true, true, 30);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 252000000.0f);
    for (int i = 0; i < 8; i++) {
        uint pin = (uint)(HDMI_BASE_PIN + i);
        pio_gpio_init(VPIO, pin);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(VPIO, VSM, HDMI_BASE_PIN, 8, true);
    pio_sm_init(VPIO, VSM, offset, &c);

    v_line = 1;            // the two pre-loaded transfers below are line 0's parts
    part_b = false;
    dma_pong = false;
    dma_channel_config d = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&d, DMACH_PONG);
    channel_config_set_dreq(&d, pio_get_dreq(VPIO, VSM, true));
    dma_channel_configure(DMACH_PING, &d, &VPIO->txf[VSM], blank_line[0], H_BLANK * 2, false);
    d = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&d, DMACH_PING);
    channel_config_set_dreq(&d, pio_get_dreq(VPIO, VSM, true));
    dma_channel_configure(DMACH_PONG, &d, &VPIO->txf[VSM], blank_active[0], H_ACTIVE * 2, false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);
    pio_sm_set_enabled(VPIO, VSM, true);

    encode_loop();         // never returns: core 1 encodes rows from here on
}

uint32_t hdmi_frames(void) { return frames; }
uint32_t hdmi_late_lines(void) { return late; }
uint32_t hdmi_irq_missed(void) { return irq_missed; }
void hdmi_core1_cycles(uint32_t *irq, uint32_t *enc) { *irq = irq_cyc; *enc = enc_cyc; }
