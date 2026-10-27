// Reading the framebuffer rows (amiga.h): the reference expansion every
// scanout and the host bench follow.
#include <string.h>
#include "amiga.h"

void amiga_fb_line(const amiga_row_t *row, int sub, uint16_t out[AMIGA_OUT_W]) {
    switch (row->mode) {
    case AMIGA_ROW_IDX8:
        for (int i = 0; i < AMIGA_OUT_W; i++) out[i] = row->pal[row->px.idx[i] & 63];
        break;
    case AMIGA_ROW_LACE4: {
        const uint16_t *pal = row->pal + 16 * (sub & 1);
        const uint8_t *n = row->px.nib[sub & 1];
        for (int x = 0; x < AMIGA_FB_W; x++) {
            out[2 * x] = pal[n[x] >> 4];
            out[2 * x + 1] = pal[n[x] & 15];
        }
        break;
    }
    default:
        for (int x = 0; x < AMIGA_FB_W; x++) out[2 * x] = out[2 * x + 1] = row->px.rgb[x];
        break;
    }
}

void amiga_fb_fill(amiga_row_t *fb, uint16_t colour) {
    for (int r = 0; r < AMIGA_FB_H; r++) {
        fb[r].mode = AMIGA_ROW_DIRECT;
        for (int x = 0; x < AMIGA_FB_W; x++) fb[r].px.rgb[x] = colour;
    }
}
