#pragma once
#include <stdint.h>

// Start 720x576@50 HDMI scanout of an AMIGA_FB_W x AMIGA_FB_H 0x0RGB
// framebuffer.  Call on the core that will own the DMA IRQ (core 1).
void video_start(const uint16_t *framebuffer);

// Number of output frames completed (bumped at the end of each 605-line frame).
uint32_t video_frames(void);
