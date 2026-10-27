// Bitplane prefetch for the core (amiga_config_t.bpl_prefetch): two DMA
// channels copy a line's plane words from chip RAM (PSRAM) into SRAM while the
// emulated CPU runs the line.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// chip_ram: the core's chip RAM pointer (the cached PSRAM window, so the DMA
// sees what the CPU wrote even while it is still in the XIP cache).
void bpl_dma_init(const uint8_t *chip_ram);
bool bpl_dma_prefetch(void *ctx, const uint32_t *src, int n, uint32_t bytes, void *dst);

#ifdef __cplusplus
}
#endif
