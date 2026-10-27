// Checks the PIO TMDS word tables (src/pico/tmds_pio.h) by decoding them the
// way a receiver would: unpack the pin patterns, check every pair is truly
// differential, recover the 10-bit characters, TMDS-decode them and compare
// with the intended levels; check DC balance of every pixel pair and the
// control-period symbols.
#include <stdio.h>
#include <stdlib.h>
#include "tmds_pio.h"

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

int main(void) {
    static uint32_t tab_r[16][4], tab_gb[256][4];
    tmds_build_tables(tab_r, tab_gb);

    uint16_t px[4096];
    for (int c = 0; c < 4096; c++) px[c] = (uint16_t)c;
    static uint32_t row[4096 * 4];
    tmds_encode_row(row, px, 4096, tab_r, tab_gb);

    for (int c = 0; c < 4096; c++) {
        const uint32_t *w = row + c * 4;
        int nib[3] = { c & 15, (c >> 4) & 15, (c >> 8) & 15 };     // lane 0 blue, 1 green, 2 red
        for (int lane = 0; lane < 3; lane++) {
            uint16_t a = lane_sym(w, lane), b = lane_sym(w + 2, lane);
            int want = tmds_level(nib[lane]);
            CHECK(tmds_decode(a) == want, "colour %03X lane %d: first decodes %02X, want %02X", c, lane, tmds_decode(a), want);
            int db = tmds_decode(b) - want;
            CHECK(db >= -1 && db <= 1, "colour %03X lane %d: second decodes %02X", c, lane, tmds_decode(b));
            CHECK(tmds_ones(a) + tmds_ones(b) == 10, "colour %03X lane %d: pair not DC-balanced", c, lane);
        }
    }

    // Different neighbours (hires): every pair of nibble levels on every lane.
    static uint32_t mix[3][16][16][4];
    tmds_build_mixed(mix);
    int maxdev = 0;
    for (int lane = 0; lane < 3; lane++)
        for (int a = 0; a < 16; a++)
            for (int b = 0; b < 16; b++) {
                const uint32_t *w = mix[lane][a][b];
                uint16_t ca = lane_sym(w, lane), cb = lane_sym(w + 2, lane);
                CHECK(tmds_decode(ca) == tmds_level(a), "mixed %X/%X lane %d: first %02X", a, b, lane, tmds_decode(ca));
                int d = tmds_decode(cb) - tmds_level(b); if (d < 0) d = -d;
                if (d > maxdev) maxdev = d;
                CHECK(d <= 4, "mixed %X/%X lane %d: second off by %d", a, b, lane, d);
                CHECK(tmds_ones(ca) + tmds_ones(cb) == 10, "mixed %X/%X lane %d: not balanced", a, b, lane);
            }
    // tmds_encode_pairs on a line mixing equal and different neighbours.
    static uint16_t lft[64], rgt[64];
    static uint32_t line[64 * 4];
    for (int i = 0; i < 64; i++) { lft[i] = (uint16_t)(i * 67 & 0xFFF); rgt[i] = (i & 1) ? lft[i] : (uint16_t)(i * 151 & 0xFFF); }
    tmds_encode_pairs(line, lft, rgt, 64, tab_r, tab_gb, mix);
    for (int i = 0; i < 64; i++) {
        int pa[3] = { lft[i] & 15, (lft[i] >> 4) & 15, lft[i] >> 8 };
        for (int lane = 0; lane < 3; lane++)
            CHECK(tmds_decode(lane_sym(line + 4 * i, lane)) == tmds_level(pa[lane]), "encode_pairs px %d lane %d", i, lane);
    }

    static const uint16_t ctrl[4] = { TMDS_CTRL_00, TMDS_CTRL_01, TMDS_CTRL_10, TMDS_CTRL_11 };
    for (int vs = 0; vs < 2; vs++)
        for (int hs = 0; hs < 2; hs++) {
            uint32_t w[2];
            tmds_ctrl_words(vs, hs, w);
            CHECK(lane_sym(w, 0) == ctrl[(vs << 1) | hs], "ctrl vs %d hs %d lane 0", vs, hs);
            CHECK(lane_sym(w, 1) == TMDS_CTRL_00 && lane_sym(w, 2) == TMDS_CTRL_00, "ctrl lanes 1/2");
            CHECK(!(w[0] >> 30) && !(w[1] >> 30), "bits 30/31 must stay clear");
        }

    if (fails) { printf("tmds_test: %d failures\n", fails); return 1; }
    printf("tmds_test: 4096 doubled colours, 3x256 mixed pairs (2nd within +-%d), 4 control states OK\n", maxdev);
    return 0;
}
