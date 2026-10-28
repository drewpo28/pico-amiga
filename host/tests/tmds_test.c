// Checks the PIO TMDS word tables (src/pico/tmds_pio.h) by decoding them the
// way a receiver would: unpack the pin patterns, check every pair is truly
// differential, recover the 10-bit characters, TMDS-decode them and compare
// with the intended levels; check DC balance of every pixel pair and the
// control-period symbols.
//
// Then the PIO HDMI by lookup (src/pico/tmds_lut.h): lines built as entry
// numbers, resolved through the entry window, must give exactly the words the
// CPU encoder and the old blanking composition gave, for every row mode.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tmds_pio.h"
#include "tmds_lut.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint16_t lane_sym(const uint32_t w[2], int lane) {
    uint16_t s = 0;
    for (int t = 0; t < 10; t++) {
        uint32_t pins = (w[t / 5] >> (6 * (t % 5) + tmds_lane_shift[lane])) & 3u;
        CHECK(pins == 1 || pins == 2, "lane %d t %d: pins %u not differential", lane, t, pins);
        s |= (uint16_t)((pins >> 1) << t);             // second pin carries the true bit
    }
    return s;
}

static int tmds_decode(uint16_t s) {
    uint8_t d = (uint8_t)s;
    if (s & 0x200) d = (uint8_t)~d;
    uint8_t out = d & 1;
    for (int i = 1; i < 8; i++) {
        int b = ((d >> i) ^ (d >> (i - 1))) & 1;
        if (!(s & 0x100)) b ^= 1;
        out |= (uint8_t)(b << i);
    }
    return out;
}

// ---- the lookup path against the old CPU composition ------------------------
static uint32_t rnd(void) { static uint32_t s = 12345; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static uint32_t terc4[16], pre[2][10], post[2][2], lead[10];
enum { HDMI_PK_N = 5 };                      // hdmi_audio.h HDMI_PK_FIXED
static uint8_t  fixed_nibs[HDMI_PK_N][2][32][3];
static void fixed_nib(int id, bool vs, uint8_t nib[32][3]) { memcpy(nib, fixed_nibs[id][vs], 32 * 3); }

static void old_char(uint32_t c, uint32_t w[2]) { tl_char_words(c, w); }

// The old video_pio.c blanking: control periods, the island at pixel 12, the lead.
static void old_blank(uint32_t *d, bool vsync, bool active, const uint8_t nib[32][3],
                      uint32_t terc_w[3][16][2]) {
    const int vlev = vsync ? 0 : 1;
    uint32_t w[2];
    int px = 0;
    tmds_ctrl_words(vlev, 1, w); for (int i = 0; i < 12; i++, px++) { d[2 * px] = w[0]; d[2 * px + 1] = w[1]; }
    tmds_ctrl_words(vlev, 0, w); for (int i = 0; i < 64; i++, px++) { d[2 * px] = w[0]; d[2 * px + 1] = w[1]; }
    tmds_ctrl_words(vlev, 1, w); for (int i = 0; i < 36; i++, px++) { d[2 * px] = w[0]; d[2 * px + 1] = w[1]; }
    uint32_t *isl = d + 2 * 12;
    for (int i = 0; i < 10; i++, isl += 2) old_char(pre[vsync][i], isl);
    for (int t = 0; t < 32; t++, isl += 2) {
        isl[0] = terc_w[0][nib[t][0]][0] | terc_w[1][nib[t][1]][0] | terc_w[2][nib[t][2]][0];
        isl[1] = terc_w[0][nib[t][0]][1] | terc_w[1][nib[t][1]][1] | terc_w[2][nib[t][2]][1];
    }
    for (int i = 0; i < 2; i++, isl += 2) old_char(post[vsync][i], isl);
    if (active) for (int i = 0; i < 10; i++) old_char(lead[i], d + 2 * (112 - 10 + i));
}

// The old video_pio.c encode_line, one output line of a row.
static void old_active(uint32_t *dst, const amiga_row_t *row, int sub,
                       const uint32_t tab_r[16][4], const uint32_t tab_gb[256][4], const uint32_t mix[3][16][16][4]) {
    static uint16_t l[AMIGA_FB_W], r[AMIGA_FB_W];
    if (row->mode == AMIGA_ROW_IDX8) {
        for (int x = 0; x < AMIGA_FB_W; x++) { l[x] = row->pal[row->px.idx[2 * x] & 63]; r[x] = row->pal[row->px.idx[2 * x + 1] & 63]; }
        tmds_encode_pairs(dst, l, r, AMIGA_FB_W, tab_r, tab_gb, mix);
    } else if (row->mode == AMIGA_ROW_LACE4) {
        const uint16_t *pal = row->pal + 16 * sub;
        for (int x = 0; x < AMIGA_FB_W; x++) { l[x] = pal[row->px.nib[sub][x] >> 4]; r[x] = pal[row->px.nib[sub][x] & 15]; }
        tmds_encode_pairs(dst, l, r, AMIGA_FB_W, tab_r, tab_gb, mix);
    } else {
        tmds_encode_row(dst, row->px.rgb, AMIGA_FB_W, tab_r, tab_gb);
    }
}

static void random_row(amiga_row_t *row, int mode) {
    memset(row, 0, sizeof *row);
    row->mode = (uint8_t)mode;
    for (int i = 0; i < 64; i++) row->pal[i] = (uint16_t)(rnd() & 0xFFF);
    uint32_t v = 0;
    for (int x = 0; x < AMIGA_OUT_W; x++) {
        if (rnd() % 4 == 0) v = rnd();                   // runs, as on a real screen
        if (mode == AMIGA_ROW_DIRECT && x < AMIGA_FB_W) row->px.rgb[x] = (uint16_t)(v & 0xFFF);
        if (mode == AMIGA_ROW_IDX8) row->px.idx[x] = (uint8_t)(v % 64);
        if (mode == AMIGA_ROW_LACE4 && x < AMIGA_FB_W) { row->px.nib[0][x] = (uint8_t)v; row->px.nib[1][x] = (uint8_t)(v >> 8); }
    }
}

static int lut_check(const uint32_t tab_r[16][4], const uint32_t tab_gb[256][4], const uint32_t mix[3][16][16][4]) {
    static tl_entry_t lut[TL_ENTRIES];
    static tl_t t;
    for (int i = 0; i < 16; i++) terc4[i] = rnd() & 0x3FF;
    for (int vs = 0; vs < 2; vs++) {
        for (int i = 0; i < 10; i++) pre[vs][i] = rnd() & 0x3FFFFFFF;
        for (int i = 0; i < 2; i++) post[vs][i] = rnd() & 0x3FFFFFFF;
    }
    for (int i = 0; i < 10; i++) lead[i] = rnd() & 0x3FFFFFFF;
    for (int id = 0; id < HDMI_PK_N; id++)
        for (int vs = 0; vs < 2; vs++)
            for (int k = 0; k < 32; k++)
                for (int l = 0; l < 3; l++) fixed_nibs[id][vs][k][l] = (uint8_t)(rnd() & 15);
    tl_init(&t, lut, tab_r, tab_gb, mix, terc4, pre, post, lead, fixed_nib, HDMI_PK_N);

    static uint32_t want[2 * 832], got[2 * 832];
    static uint16_t items[TL_LINE];
    static amiga_row_t row;
    int lines = 0;
    for (int n = 0; n < 3000; n++, lines++) {
        const int page = n & 3, mode = n % 3;
        const bool vsync = n % 7 == 0, active = n % 5 != 0;
        const int sub = (n >> 2) & 1;
        uint8_t nib[32][3];
        const int id = (n % 3 == 0) ? -1 : (int)(rnd() % HDMI_PK_N);
        if (id < 0) for (int k = 0; k < 32; k++) for (int l = 0; l < 3; l++) nib[k][l] = (uint8_t)(rnd() & 15);
        else memcpy(nib, fixed_nibs[id][vsync], sizeof nib);
        random_row(&row, mode);

        old_blank(want, vsync, active, nib, t.terc);
        if (active) old_active(want + 2 * 112, &row, sub, tab_r, tab_gb, mix);
        else { uint32_t w[2]; tmds_ctrl_words(vsync ? 0 : 1, 1, w); for (int x = 112; x < 832; x++) { want[2 * x] = w[0]; want[2 * x + 1] = w[1]; } }

        int used = 0;
        tl_blank(items, vsync, active);
        if (id >= 0) tl_island_fixed(items, id, vsync); else tl_island_audio(&t, items, page, &used, nib);
        if (active) tl_active(&t, items, page, &used, &row, sub); else tl_active_blank(items, vsync);
        CHECK(used <= TL_PAGE, "line %d: %d entries", n, used);
        for (int s = 0; s < TL_LINE; s++) {
            CHECK(items[s] < TL_ENTRIES, "line %d slot %d: entry %u", n, s, items[s]);
            memcpy(got + 4 * s, lut[items[s]], 16);
        }
        for (int i = 0; i < 2 * 832; i++)
            if (got[i] != want[i]) { CHECK(0, "line %d (mode %d vs %d act %d id %d): word %d (px %d)", n, mode, vsync, active, id, i, i / 2); break; }
    }
    return lines;
}

// The doubled and mixed tables of one mode.  capture (Capture card
// compatibility): every character is balanced by itself and a doubled pixel is
// one character twice, a mixed pair the two levels exactly.
static int tables_check(bool capture, uint32_t tab_r[16][4], uint32_t tab_gb[256][4], uint32_t mix[3][16][16][4]) {
    tmds_build_tables(tab_r, tab_gb, capture);

    uint16_t px[4096];
    for (int c = 0; c < 4096; c++) px[c] = (uint16_t)c;
    static uint32_t row[4096 * 4];
    tmds_encode_row(row, px, 4096, tab_r, tab_gb);

    for (int c = 0; c < 4096; c++) {
        const uint32_t *w = row + c * 4;
        int nib[3] = { c & 15, (c >> 4) & 15, (c >> 8) & 15 };     // lane 0 blue, 1 green, 2 red
        for (int lane = 0; lane < 3; lane++) {
            uint16_t a = lane_sym(w, lane), b = lane_sym(w + 2, lane);
            int want = tmds_level(nib[lane], capture);
            CHECK(tmds_decode(a) == want, "colour %03X lane %d: first decodes %02X, want %02X", c, lane, tmds_decode(a), want);
            int db = tmds_decode(b) - want;
            CHECK(db >= -1 && db <= 1, "colour %03X lane %d: second decodes %02X", c, lane, tmds_decode(b));
            CHECK(tmds_ones(a) + tmds_ones(b) == 10, "colour %03X lane %d: pair not DC-balanced", c, lane);
            if (capture) CHECK(a == b && tmds_ones(a) == 5, "capture: colour %03X lane %d: %03X %03X", c, lane, a, b);
        }
    }
    if (capture)
        for (int n = 0; n < 16; n++) {
            int d = tmds_level(n, true) - tmds_level(n, false); if (d < 0) d = -d;
            CHECK(d <= 2, "capture: nibble %X moves by %d", n, d);
        }

    // Different neighbours (hires): every pair of nibble levels on every lane.
    tmds_build_mixed(mix, capture);
    int maxdev = 0;
    for (int lane = 0; lane < 3; lane++)
        for (int a = 0; a < 16; a++)
            for (int b = 0; b < 16; b++) {
                const uint32_t *w = mix[lane][a][b];
                uint16_t ca = lane_sym(w, lane), cb = lane_sym(w + 2, lane);
                CHECK(tmds_decode(ca) == tmds_level(a, capture), "mixed %X/%X lane %d: first %02X", a, b, lane, tmds_decode(ca));
                int d = tmds_decode(cb) - tmds_level(b, capture); if (d < 0) d = -d;
                if (d > maxdev) maxdev = d;
                CHECK(d <= (capture ? 0 : 4), "mixed %X/%X lane %d: second off by %d", a, b, lane, d);
                CHECK(tmds_ones(ca) + tmds_ones(cb) == 10, "mixed %X/%X lane %d: not balanced", a, b, lane);
                if (capture) CHECK(tmds_ones(ca) == 5 && tmds_ones(cb) == 5, "capture: mixed %X/%X lane %d", a, b, lane);
            }
    // tmds_encode_pairs on a line mixing equal and different neighbours.
    static uint16_t lft[64], rgt[64];
    static uint32_t line[64 * 4];
    for (int i = 0; i < 64; i++) { lft[i] = (uint16_t)(i * 67 & 0xFFF); rgt[i] = (i & 1) ? lft[i] : (uint16_t)(i * 151 & 0xFFF); }
    tmds_encode_pairs(line, lft, rgt, 64, tab_r, tab_gb, mix);
    for (int i = 0; i < 64; i++) {
        int pa[3] = { lft[i] & 15, (lft[i] >> 4) & 15, lft[i] >> 8 };
        for (int lane = 0; lane < 3; lane++)
            CHECK(tmds_decode(lane_sym(line + 4 * i, lane)) == tmds_level(pa[lane], capture), "encode_pairs px %d lane %d", i, lane);
    }
    return maxdev;
}

int main(void) {
    static uint32_t tab_r[16][4], tab_gb[256][4];
    static uint32_t mix[3][16][16][4];
    tables_check(true, tab_r, tab_gb, mix);
    const int lines_capture = lut_check(tab_r, tab_gb, mix);
    const int maxdev = tables_check(false, tab_r, tab_gb, mix);

    static const uint16_t ctrl[4] = { TMDS_CTRL_00, TMDS_CTRL_01, TMDS_CTRL_10, TMDS_CTRL_11 };
    for (int vs = 0; vs < 2; vs++)
        for (int hs = 0; hs < 2; hs++) {
            uint32_t w[2];
            tmds_ctrl_words(vs, hs, w);
            CHECK(lane_sym(w, 0) == ctrl[(vs << 1) | hs], "ctrl vs %d hs %d lane 0", vs, hs);
            CHECK(lane_sym(w, 1) == TMDS_CTRL_00 && lane_sym(w, 2) == TMDS_CTRL_00, "ctrl lanes 1/2");
            CHECK(!(w[0] >> 30) && !(w[1] >> 30), "bits 30/31 must stay clear");
        }

    const int lines = lut_check(tab_r, tab_gb, mix);

    if (fails) { printf("tmds_test: %d failures\n", fails); return 1; }
    printf("tmds_test: 4096 doubled colours, 3x256 mixed pairs (2nd within +-%d), 4 control states,"
           " %d lookup lines OK; capture mode: single characters, %d lookup lines OK\n", maxdev, lines, lines_capture);
    return 0;
}
