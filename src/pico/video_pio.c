// DVI/HDMI output through a PIO TMDS serializer, for boards whose HDMI pins are
// not the HSTX ones (PICO_DV: clock on GPIO 6/7, data on 8-13).
//
// Video > Output > Mode (video_modes.c), 720x576 @ 50.06 Hz (25.2 MHz pixel, 832 x 605)
// by default, so the PIO runs at 252 MHz: clk_sys / 1.5 at 378, / 2 at 504
// (pico-speccy runs its PIO HDMI from the same half-integer dividers); 800x600
// at 37.8 MHz (378 / 1.0) or 33.6 MHz (504 / 1.5); 640x480 @60 at 25.2 MHz.
//
// Every output pixel is two 32-bit words of pin patterns (tmds_pio.h), and a
// line goes out by lookup (tmds_lut.h, pico-speccy's PIO HDMI scheme): it is
// 416 entry numbers, one per two pixels, and a chain of DMA channels and a
// second state machine turns them into words.
//   line_ctrl  writes the next line's address (a ring of four buffers) into
//              line's trigger alias; its completion IRQ only counts lines
//   line       416 numbers -> the converter SM (16-bit transfers)
//   converter  number -> window | number << 4 (SM_CONV, the window base in X)
//   lut_ctrl   that address -> lut's trigger alias
//   lut        the entry's four words -> the TMDS SM, then back to lut_ctrl
// The chain runs by itself; nothing has to be done within a line's blanking.
// Core 1's loop builds the lines (numbers, and an entry for each colour or
// hires pair new on the line) up to two ahead, renders the emulator's lines
// in between (amiga_render_pump), and the old CPU encoder's ~6 ms a frame and
// its per-line island work in an IRQ with a 4.4 us deadline are gone.
#include "video.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/bus_ctrl.h"
#include "amiga.h"
#include "ui/ui.h"
#include "tmds_pio.h"
#include "tmds_lut.h"
#include "tmds.pio.h"
#include "hdmi_audio.h"
#include "board.h"

#ifndef HDMI_BASE_PIN
#define HDMI_BASE_PIN 6
#endif
// The clock pair and the 6 data pins (tmds_pio.h): PICO_DV and the rest put the
// clock first, ZERO2 after the data (pico-speccy's hdmi.h).
#ifdef ZERO2
#define PIN_CLK   (HDMI_BASE_PIN + 6)
#define PIN_DATA  HDMI_BASE_PIN
#else
#define PIN_CLK   HDMI_BASE_PIN
#define PIN_DATA  (HDMI_BASE_PIN + 2)
#endif

// The mode's vertical timing (video_timing(), fixed while the scanout runs).
static uint32_t V_FP, V_SYNC, V_ACTIVE, V_TOTAL;
static uint32_t pace_lines;          // output lines per emulated frame (hdmi_frame_lines)

#define VPIO       pio0
#define VSM        0                 // TMDS serializer
#define SM_CONV    1                 // number -> entry address
#define CH_LINE_CTRL 0               // main.c claims 0 and 1 for the scanout
#define CH_LINE      1
#define LEAD       2                 // lines built ahead of the one starting

// The entry window: first in RAM, 64 KB-aligned (src/pico/ld/), not zeroed by crt0.
static tl_entry_t hdmi_lut_window[TL_ENTRIES];
static tl_t tl;
static uint32_t tab_r[16][4];
static uint32_t tab_gb[256][4];
static uint32_t tab_mix[3][16][16][4];               // hires pairs of different colours

// VGA's working memory when it runs instead (video_vga.c): one scanout runs
// after video_select(), and the window is the largest buffer here.
void *hdmi_spare(uint32_t size) { return size <= sizeof hdmi_lut_window ? (void *)hdmi_lut_window : NULL; }

static uint16_t items[4][TL_LINE_MAX] __attribute__((aligned(4)));
static const uint16_t *line_ring[4] __attribute__((aligned(16)));   // line_ctrl reads it round
static uint8_t  slot_kind[4] = { 0xFF, 0xFF, 0xFF, 0xFF };   // vsync | active << 1 | picture << 2
static volatile uint32_t built[4];                   // the line in items[i]

static const amiga_row_t *fb;
static int      ch_lut_ctrl, ch_lut;
static volatile uint32_t started;                    // lines the DMA has begun
static volatile uint32_t frames;
static uint32_t v_isr;                               // vertical position of the line begun
static uint32_t next_build;                          // the line to build next
static int      v_build;                             // ...and its vertical position
static bool     prev_lace;                           // the row of the line before was LACE4
static volatile uint32_t late;                       // lines begun before they were built
static volatile uint32_t stalls;                     // lines the serializer ran dry in (FDEBUG TXSTALL)
static volatile uint32_t irq_cyc, enc_cyc;           // core 1 cycles: line IRQ, building

static void __not_in_flash_func(line_irq)(void) {
    const uint32_t t0 = board_cycles();
    dma_hw->ints0 = 1u << CH_LINE_CTRL;
    const uint32_t l = started;
    if (built[l & 3] != l) late++;
    // The serializer waited on an empty FIFO since the last line: the picture
    // tore, and so did the island if it was in it (the sink mutes on a torn
    // packet). The flag is sticky, write 1 clears it.
    const uint32_t txstall = 1u << (PIO_FDEBUG_TXSTALL_LSB + VSM);
    if (VPIO->fdebug & txstall) { VPIO->fdebug = txstall; stalls++; }
    started = l + 1;
    if (++v_isr == V_TOTAL) { v_isr = 0; frames++; }
    irq_cyc += board_cycles() - t0;
}

// Build line `next_build` into its ring slot and page.
static void __not_in_flash_func(build_line)(void) {
    const uint32_t t0 = board_cycles();
    const uint32_t l = next_build;
    const int slot = (int)(l & 3), page = (int)(l % TL_PAGES), v = v_build;
    const bool vsync = v >= (int)V_FP && v < (int)(V_FP + V_SYNC);
    const bool active = v >= (int)(V_TOTAL - V_ACTIVE);
    // The framebuffer row an active line shows, if any (the window's rows).
    const video_window_t *w = video_window();
    const int out = v - (int)(V_TOTAL - V_ACTIVE) - w->y;
    const bool picture = active && out >= 0 && out < 2 * w->rows;
    uint16_t *it = items[slot];
    int n = 0;
    const uint8_t kind = (uint8_t)(vsync | active << 1 | picture << 2);
    if (slot_kind[slot] != kind) {
        slot_kind[slot] = kind;
        tl_blank(&tl, it, vsync, active);
        if (!active) tl_active_blank(&tl, it, vsync);
        else         tl_active_black(&tl, it, picture);
    }
    uint8_t nib[32][3];
    const int id = hdmi_audio_packet(nib, active ? 0 : (uint32_t)v + 1, vsync);
    if (id >= 0) tl_island_fixed(&tl, it, id, vsync);
    else         tl_island_audio(&tl, it, page, &n, nib);
    if (picture) {
        tl.L.col = w->col;           // Picture position may move a 640x480 window
        const amiga_row_t *row = &fb[w->row + (out >> 1)];
        if ((out & 1) && !prev_lace) {
            // The second line of a row that was not interlaced when its first
            // line was built: the same numbers (entries in that line's page).
            const int at = tl.L.blank + tl.L.pic_x;      // even: whole words
            const uint32_t *s = (const uint32_t *)(const void *)(items[(slot + 3) & 3] + at);
            uint32_t *d = (uint32_t *)(void *)(it + at);
            for (int i = 0; i < tl.L.pic_w / 2; i++) d[i] = s[i];
        } else {
            prev_lace = row->mode == AMIGA_ROW_LACE4;
            tl_active(&tl, it, page, &n, row, out & 1);
        }
    }
    built[slot] = l;
    next_build = l + 1;
    if (++v_build == (int)V_TOTAL) v_build = 0;
    enc_cyc += board_cycles() - t0;
}

// Core 1, forever: keep the lines built LEAD ahead of the DMA, and render the
// emulator's lines in between (amiga_render_pump, in small steps).
static void __no_inline_not_in_flash_func(build_loop)(void) {
    for (;;) {
        if ((int32_t)(next_build - started) < LEAD) build_line();
        else                                         amiga_render_pump();
    }
}

static void set_x(PIO pio, uint sm, uint32_t v) {
    pio_sm_put_blocking(pio, sm, v);
    pio_sm_exec(pio, sm, pio_encode_pull(false, true));
    pio_sm_exec(pio, sm, pio_encode_mov(pio_x, pio_osr));
}

void hdmi_start(const amiga_row_t *framebuffer) {
    fb = framebuffer;
    const video_timing_t *t = video_timing();
    V_FP = t->v_fp; V_SYNC = t->v_sync; V_ACTIVE = t->v_active;
    V_TOTAL = t->v_fp + t->v_sync + t->v_bp + t->v_active;
    const uint32_t h_total = t->h_fp + t->h_sync + t->h_bp + t->h_active;
    // A 50 Hz mode paces the emulation by its frames; another by the 20 ms of a
    // PAL frame in its lines (640x480 @60: 630).
    pace_lines = t->hz == 50 ? V_TOTAL : t->pixel_hz / h_total / 50;
    hdmi_audio_init(t->pixel_hz, h_total, V_TOTAL);
    // Video > Output > Capture card mode (tmds_pio.h): read once, at boot.
    const bool capture = settings_hdmi_capture();
    tmds_build_tables(tab_r, tab_gb, capture);
    tmds_build_mixed(tab_mix, capture);
    uint32_t terc4[16], pre[2][10], post[2][2];
    for (int n = 0; n < 16; n++) terc4[n] = hdmi_terc4(n);
    for (int vs = 0; vs < 2; vs++) hdmi_island_frame(pre[vs], post[vs], vs);
    tl_init(&tl, hdmi_lut_window, tab_r, tab_gb, tab_mix, terc4, pre, post, hdmi_video_lead,
            hdmi_audio_fixed_nibbles, HDMI_PK_FIXED);
    // The line in slots of two pixels (all of a mode's counts are even).
    const video_window_t *w = video_window();
    tl.L.line = (int)h_total / 2;
    tl.L.fp = t->h_fp / 2;
    tl.L.sync_end = (t->h_fp + t->h_sync) / 2;
    tl.L.blank = (int)(h_total - t->h_active) / 2;
    tl.L.pic_x = w->x / 2;
    tl.L.pic_w = w->cols;
    tl.L.col = w->col;
    for (int i = 0; i < 4; i++) { line_ring[i] = items[i]; built[i] = 0xFFFFFFFFu; }
    next_build = 0; v_build = 0; started = 0; v_isr = 0;
    while (next_build < LEAD) build_line();

#if HDMI_BASE_PIN >= 32
    pio_set_gpio_base(VPIO, 16);     // ZERO2: display on GPIO 32-39
#endif
    // The serializer: two words a pixel from the TX FIFO.
    uint offset = pio_add_program(VPIO, &tmds_program);
    pio_sm_config c = tmds_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, PIN_CLK);
    sm_config_set_out_pins(&c, PIN_DATA, 6);
    sm_config_set_out_shift(&c, true, true, 30);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (10.0f * (float)t->pixel_hz));
    for (int i = 0; i < 8; i++) {
        uint pin = (uint)(HDMI_BASE_PIN + i);
        pio_gpio_init(VPIO, pin);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(VPIO, VSM, HDMI_BASE_PIN, 8, true);
    pio_sm_init(VPIO, VSM, offset, &c);

    // The converter: an entry number in (a 16-bit DMA write: both halves), the
    // entry's address out, X << 16 | number << 4 (the ISR shifts right).
    static uint16_t conv_instr[5];
    conv_instr[0] = (uint16_t)pio_encode_pull(false, true);
    conv_instr[1] = (uint16_t)pio_encode_in(pio_null, 4);
    conv_instr[2] = (uint16_t)pio_encode_in(pio_osr, 12);
    conv_instr[3] = (uint16_t)pio_encode_in(pio_x, 16);
    conv_instr[4] = (uint16_t)pio_encode_push(false, true);
    static const struct pio_program conv_prog = { .instructions = conv_instr, .length = 5, .origin = -1 };
    const uint conv_off = pio_add_program(VPIO, &conv_prog);
    pio_sm_config k = pio_get_default_sm_config();
    sm_config_set_wrap(&k, conv_off, conv_off + 4);
    sm_config_set_in_shift(&k, true, false, 32);
    sm_config_set_out_shift(&k, true, false, 32);
    pio_sm_init(VPIO, SM_CONV, conv_off, &k);
    set_x(VPIO, SM_CONV, (uint32_t)(uintptr_t)hdmi_lut_window >> 16);

    // The chain (header comment).
    ch_lut_ctrl = dma_claim_unused_channel(true);
    ch_lut = dma_claim_unused_channel(true);
    dma_channel_config d = dma_channel_get_default_config(CH_LINE_CTRL);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_ring(&d, false, 4);                 // the four ring pointers
    channel_config_set_high_priority(&d, true);
    dma_channel_configure(CH_LINE_CTRL, &d, &dma_hw->ch[CH_LINE].al3_read_addr_trig, line_ring, 1, false);

    d = dma_channel_get_default_config(CH_LINE);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_16);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(VPIO, SM_CONV, true));
    channel_config_set_chain_to(&d, CH_LINE_CTRL);
    channel_config_set_high_priority(&d, true);
    dma_channel_configure(CH_LINE, &d, &VPIO->txf[SM_CONV], items[0], (uint)tl.L.line, false);

    d = dma_channel_get_default_config((uint)ch_lut_ctrl);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(VPIO, SM_CONV, false));
    channel_config_set_high_priority(&d, true);
    dma_channel_configure((uint)ch_lut_ctrl, &d, &dma_hw->ch[ch_lut].al3_read_addr_trig,
                          &VPIO->rxf[SM_CONV], 1, false);

    d = dma_channel_get_default_config((uint)ch_lut);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(VPIO, VSM, true));
    channel_config_set_chain_to(&d, (uint)ch_lut_ctrl);
    channel_config_set_high_priority(&d, true);
    dma_channel_configure((uint)ch_lut, &d, &VPIO->txf[VSM], hdmi_lut_window, 4, false);

    dma_hw->ints0 = 1u << CH_LINE_CTRL;
    dma_hw->inte0 = 1u << CH_LINE_CTRL;
    irq_set_exclusive_handler(DMA_IRQ_0, line_irq);
    irq_set_enabled(DMA_IRQ_0, true);
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start((uint)ch_lut_ctrl);                 // waits for the converter
    pio_enable_sm_mask_in_sync(VPIO, (1u << VSM) | (1u << SM_CONV));
    dma_channel_start(CH_LINE_CTRL);

    build_loop();          // never returns: core 1 builds lines from here on
}

uint32_t hdmi_frames(void) { return frames; }
// `started` and v_isr start at 0 together; framebuffer row 0 begins at V_TOTAL -
// V_ACTIVE + the window's y.
uint32_t hdmi_lines(void) { return started + V_ACTIVE - (uint32_t)video_window()->y; }
uint32_t hdmi_frame_lines(void) { return pace_lines; }
uint32_t hdmi_late_lines(void) { return late; }
uint32_t hdmi_stalled_lines(void) { return stalls; }
void hdmi_core1_cycles(uint32_t *irq, uint32_t *enc) { *irq = irq_cyc; *enc = enc_cyc; }
