// DVI/HDMI output through the RP2350 HSTX serializer and its TMDS encoder.
//
// Mode: Video > Output > Mode (video_modes.c; 720x576 @ 50.06 Hz from a 25.2 MHz pixel
// clock, 832 x 605, by default - pico-speccy's hw-proven 576-line timing).  The
// 288 framebuffer rows (amiga.h) become 720x576: each FB row feeds two output
// lines (two different ones for an interlaced row), and each FIFO word carries
// two output pixels that the expander shifts out in turn (a doubled lowres
// pixel or a hires pair).  A larger mode (800x600) frames that in black, which
// the line buffers keep at their ends; a smaller one (640x480) shows a window.
//
// Colours are the Amiga's 12-bit 0x0RGB (direct, or via the row palette).  The TMDS lanes take
// their top four bits straight from those nibbles (NBITS = 3), so colour
// conversion costs nothing; the price is a 240/255 peak level.
//
// Scanout runs entirely from a DMA ping-pong on core 1 with one IRQ per
// transfer, after the pico-examples "dvi_out_hstx_encoder" design (BSD-3,
// Raspberry Pi).  The pin/lane map is pico-speccy's (src/drivers/hdmi/
// hdmi_word.h) for its HDMI_BASE_PIN 12 boards.
#include "video.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "amiga.h"
#include "hdmi_audio.h"

#ifndef HDMI_BASE_PIN
#define HDMI_BASE_PIN 12
#endif

// ---- timing (video_timing(), fixed while the scanout runs) ------------------------
static uint32_t H_FP, H_SYNC, H_BP, H_ACTIVE;
static uint32_t V_FP, V_SYNC, V_ACTIVE, V_TOTAL;
static uint32_t pace_lines;          // output lines per emulated frame (hdmi_frame_lines)

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define HSTX_CMD_RAW        (0x0u << 12)
#define HSTX_CMD_TMDS       (0x2u << 12)

static const uint32_t sync_word[4] = { SYNC_V0_H0, SYNC_V0_H1, SYNC_V1_H0, SYNC_V1_H1 };   // [vs << 1 | hs]

// ---- state ------------------------------------------------------------------------
#define DMACH_PING 0
#define DMACH_PONG 1

static const amiga_row_t *fb;
// Two pixels a word, the widest mode's line (800); the black either side of a
// framebuffer row is never written.
#define LINE_WORDS 400
static uint32_t linebuf[2][LINE_WORDS];
static uint32_t black_line[LINE_WORDS];         // a picture line outside the window (SRAM: DMA)
// Command lists, by line parity: the one for line L is written while line L-1
// plays from the other.  Every control period is a RAW run, one word per pixel
// (pico-speccy's hdmi_tl_raw_run): no RAW_REPEAT / TMDS_REPEAT, see build_cmd.
// Longest: a blank line of the widest h_total (800x600 framed: 1600) + 4 commands.
#define CMD_WORDS (1600 + 4)
static uint32_t cmdbuf[2][CMD_WORDS];
static volatile uint32_t frames;
static volatile uint32_t lines;              // v_line advances (v_line = lines + 2)
static bool     dma_pong;
static uint32_t v_line;              // next output line to post
static bool     cmd_posted;

// One output line of a row (amiga.h formats) as FIFO words, two output pixels
// each: p | p << 16 doubles a lowres pixel, two indices give a hires pair.  The
// window's columns from its col.
static void __not_in_flash_func(expand_line)(uint32_t *dst, const amiga_row_t *row, int sub,
                                             int col, int cols) {
    switch (row->mode) {
    case AMIGA_ROW_IDX8: {
        const uint8_t *ix = row->px.idx + 2 * col;
        const uint16_t *pal = row->pal;
        for (int x = 0; x < cols; x++)
            dst[x] = pal[ix[2 * x] & 63] | ((uint32_t)pal[ix[2 * x + 1] & 63] << 16);
        break;
    }
    case AMIGA_ROW_LACE4: {
        const uint8_t *n = row->px.nib[sub] + col;
        const uint16_t *pal = row->pal + 16 * sub;
        for (int x = 0; x < cols; x++)
            dst[x] = pal[n[x] >> 4] | ((uint32_t)pal[n[x] & 15] << 16);
        break;
    }
    default: {
        const uint16_t *src = row->px.rgb + col;
        for (int x = 0; x < cols; x += 4) {
            uint32_t a = src[x], b = src[x + 1], c = src[x + 2], d = src[x + 3];
            dst[x]     = a | (a << 16);
            dst[x + 1] = b | (b << 16);
            dst[x + 2] = c | (c << 16);
            dst[x + 3] = d | (d << 16);
        }
        break;
    }
    }
}

// The framebuffer row output line `l` shows (sub = its upper / lower line), or
// -1: a black line, or not a picture line at all.
static inline int __not_in_flash_func(fb_out)(uint32_t l, int *sub) {
    if (l < V_TOTAL - V_ACTIVE) return -1;
    const video_window_t *w = video_window();
    const int out = (int)(l - (V_TOTAL - V_ACTIVE)) - w->y;
    if (out < 0 || out >= 2 * w->rows) return -1;
    *sub = out & 1;
    return w->row + (out >> 1);
}

// `n` pixels of one control word as a RAW block, one word per pixel.
static inline uint32_t *__not_in_flash_func(raw_run)(uint32_t *p, uint32_t n, uint32_t word) {
    *p++ = HSTX_CMD_RAW | n;
    for (uint32_t i = 0; i < n; i++) *p++ = word;
    return p;
}

// The command list of line `l`: sync, its data island and, on an active line,
// the video preamble/guard band and the TMDS command for its pixels (which the
// next transfer carries: the row's, or black_line).  No *_REPEAT commands: with
// RAW_REPEAT | 688 for a blank line's back porch + width, the blank lines took
// ~300 px instead of 800 (hw 2026-10-29, PCp2: 63 Hz, no picture), and split
// into runs of 255 the stream fell apart altogether.  pico-speccy, which works on
// the same boards, paces its blank lines word per pixel.
static uint32_t __not_in_flash_func(build_cmd)(uint32_t *d, uint32_t l) {
    const bool active = l >= V_TOTAL - V_ACTIVE;
    const bool vsync = l >= V_FP && l < V_FP + V_SYNC;
    const uint32_t vs = vsync ? 0u : 1u;
    uint32_t *p = d;
    p = raw_run(p, H_FP, sync_word[vs << 1 | 1]);
    *p++ = HSTX_CMD_RAW | HDMI_ISLAND_PX;
    hdmi_audio_island(p, active ? 0 : l + 1, vsync); p += HDMI_ISLAND_PX;
    p = raw_run(p, H_SYNC - HDMI_ISLAND_PX, sync_word[vs << 1]);
    if (active) {
        p = raw_run(p, H_BP - HDMI_VLEAD_PX, SYNC_V1_H1);
        *p++ = HSTX_CMD_RAW | HDMI_VLEAD_PX;
        for (int i = 0; i < HDMI_VLEAD_PX; i++) *p++ = hdmi_video_lead[i];
        *p++ = HSTX_CMD_TMDS | H_ACTIVE;
    } else {
        p = raw_run(p, H_BP + H_ACTIVE, sync_word[vs << 1 | 1]);
    }
    return (uint32_t)(p - d);
}

static void __not_in_flash_func(dma_irq)(void) {
    uint ch = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *c = &dma_hw->ch[ch];
    dma_hw->intr = 1u << ch;
    dma_pong = !dma_pong;

    if (v_line < V_TOTAL - V_ACTIVE) {
        uint32_t *d = cmdbuf[v_line & 1];
        c->read_addr = (uintptr_t)d;
        c->transfer_count = build_cmd(d, v_line);
    } else if (!cmd_posted) {
        uint32_t *d = cmdbuf[v_line & 1];
        c->read_addr = (uintptr_t)d;
        c->transfer_count = build_cmd(d, v_line);
        cmd_posted = true;
        // Prepare the pixels this command list leads into.  The previous line's
        // pixels are being sent from the other buffer meanwhile.
        int sub;
        const int row = fb_out(v_line, &sub);
        if (row >= 0) {
            const video_window_t *w = video_window();
            expand_line(linebuf[v_line & 1] + w->x / 2, &fb[row], sub, w->col, w->cols);
        }
    } else {
        int sub;
        c->read_addr = fb_out(v_line, &sub) >= 0 ? (uintptr_t)linebuf[v_line & 1]
                                                 : (uintptr_t)black_line;
        c->transfer_count = H_ACTIVE / 2;
        cmd_posted = false;
    }
    if (!cmd_posted) {
        lines++;
        if (++v_line == V_TOTAL) { v_line = 0; frames++; }
    }
}

static void hstx_setup(uint32_t pixel_hz) {
    const uint32_t sys = clock_get_hz(clk_sys);
    const uint32_t want = 5 * pixel_hz;              // 5 clk_hstx per pixel
    uint32_t div = (sys + want / 2) / want;
    if (div < 1) div = 1;
    clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys, div);

    // TMDS encoder input: 0x0RGB nibbles. Lane 2 = red (bits 11:8), lane 1 =
    // green (7:4), lane 0 = blue (3:0); each rotated so its nibble sits at 7:4.
    hstx_ctrl_hw->expand_tmds =
        3  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 4  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        3  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 0  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        3  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 28 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    // Two pixels per FIFO word (the same pixel twice: horizontal doubling).
    hstx_ctrl_hw->expand_shift =
        2  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_CLKPHASE_LSB |            // clock starts high (pico-speccy)
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Pin map (offsets from HDMI_BASE_PIN): clock pair at 0/1, data pairs from 2,
    // each pair wired N/P (first pin inverted).  PICO_PC swaps red and green.
#ifdef PICO_PC
    static const int lane_bit[3] = { 2, 6, 4 };
#else
    static const int lane_bit[3] = { 2, 4, 6 };
#endif
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (int lane = 0; lane < 3; lane++) {
        uint32_t sel = (uint32_t)(lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
                       (uint32_t)(lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[lane_bit[lane]]     = sel | HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[lane_bit[lane] + 1] = sel;
    }
    for (int i = 0; i < 8; i++) {
        uint pin = (uint)(HDMI_BASE_PIN + i);
        gpio_set_function(pin, GPIO_FUNC_HSTX);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    }
}

void hdmi_start(const amiga_row_t *framebuffer) {
    fb = framebuffer;
    const video_timing_t *t = video_timing();
    H_FP = t->h_fp; H_SYNC = t->h_sync; H_BP = t->h_bp; H_ACTIVE = t->h_active;
    V_FP = t->v_fp; V_SYNC = t->v_sync; V_ACTIVE = t->v_active;
    V_TOTAL = t->v_fp + t->v_sync + t->v_bp + t->v_active;
    const uint32_t h_total = H_FP + H_SYNC + H_BP + H_ACTIVE;
    // A 50 Hz mode paces the emulation by its frames; another by the 20 ms of a
    // PAL frame in its lines (640x480 @60: 630, a whole number).
    pace_lines = t->hz == 50 ? V_TOTAL : t->pixel_hz / h_total / 50;
    hdmi_audio_init(t->pixel_hz, h_total, V_TOTAL);
    v_line = 2;                      // the two channels below are pre-loaded with lines 0 and 1
    cmd_posted = false;
    dma_pong = false;
    hstx_setup(t->pixel_hz);

    // Lines 0 and 1 (vertical front porch) are pre-loaded; the IRQ takes over at
    // line 2, whose list replaces line 0's once that has been read.
    const uint32_t n0 = build_cmd(cmdbuf[0], 0), n1 = build_cmd(cmdbuf[1], 1);
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, cmdbuf[0], n0, false);
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, cmdbuf[1], n1, false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);
}

uint32_t hdmi_frames(void) { return frames; }
// Framebuffer row 0 begins at v_line V_TOTAL - V_ACTIVE + the window's y (a
// smaller mode's first row is no row 0: main.c's 70% then lands a little early).
uint32_t hdmi_lines(void) { return lines + 2 + V_ACTIVE - (uint32_t)video_window()->y; }
uint32_t hdmi_frame_lines(void) { return pace_lines; }
uint32_t hdmi_late_lines(void) { return 0; }
uint32_t hdmi_stalled_lines(void) { return 0; }
void hdmi_core1_cycles(uint32_t *irq, uint32_t *enc) { *irq = *enc = 0; }
