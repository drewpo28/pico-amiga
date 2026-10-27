#pragma once
#include <stdint.h>
#include "amiga.h"

// Start 720x576@50 HDMI scanout of the AMIGA_FB_H framebuffer rows (amiga.h).
// Call on the core that will own the DMA IRQ (core 1).
void video_start(const amiga_row_t *framebuffer);

// Number of output frames completed (bumped at the end of each 605-line frame).
uint32_t video_frames(void);
