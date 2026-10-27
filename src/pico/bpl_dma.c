// Bitplane prefetch (bpl_dma.h).  A data channel copies one block per plane;
// a control channel feeds it control blocks {word count, read address} through
// its AL3 TRANS_COUNT / READ_ADDR_TRIG pair, the data channel chaining back to
// it after each block.  The write address simply runs on, so the blocks land
// one after another at dst.  The last real block copies a 1 right after the
// data (the core's "done" word); the block after it, {0, NULL}, is a null
// trigger, which stops the chain.
//
// The reads go through the cached PSRAM window: the XIP cache is write-back,
// and the uncached window would miss what the CPU wrote but not yet evicted.
#include "bpl_dma.h"
#include "hardware/dma.h"
#include "hardware/sync.h"

static int ch_ctl = -1, ch_dat = -1;
static const uint8_t *chip;
static struct { uint32_t count; const void *read; } cb[6 + 2] __attribute__((aligned(8)));
static uint32_t one = 1;                  // in SRAM, not flash

void bpl_dma_init(const uint8_t *chip_ram) {
    chip = chip_ram;
    ch_ctl = dma_claim_unused_channel(true);
    ch_dat = dma_claim_unused_channel(true);

    dma_channel_config d = dma_channel_get_default_config((uint)ch_dat);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, true);
    channel_config_set_chain_to(&d, (uint)ch_ctl);
    dma_channel_configure((uint)ch_dat, &d, NULL, NULL, 0, false);

    dma_channel_config c = dma_channel_get_default_config((uint)ch_ctl);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, 3);            // write wraps over the 8-byte register pair
    dma_channel_configure((uint)ch_ctl, &c, &dma_hw->ch[ch_dat].al3_transfer_count, cb, 2, false);
}

bool __not_in_flash_func(bpl_dma_prefetch)(void *ctx, const uint32_t *src, int n, uint32_t bytes, void *dst) {
    (void)ctx;
    if (ch_ctl < 0 || n < 1 || n > 6) return false;
    if (dma_hw->ch[ch_ctl].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) return false;
    if (dma_hw->ch[ch_dat].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) return false;
    for (int i = 0; i < n; i++) { cb[i].count = bytes / 4; cb[i].read = chip + src[i]; }
    cb[n].count = 1;     cb[n].read = &one;
    cb[n + 1].count = 0; cb[n + 1].read = NULL;
    __compiler_memory_barrier();
    dma_hw->ch[ch_dat].write_addr = (uintptr_t)dst;
    dma_hw->ch[ch_ctl].al3_read_addr_trig = (uintptr_t)cb;
    return true;
}
