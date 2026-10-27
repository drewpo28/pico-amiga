#pragma once
#include <stdint.h>

#define PSRAM_BASE 0x11000000u

// Bring up the QSPI PSRAM on XIP CS1 at `cs_pin` (8 on RP2350A carriers, 47 on
// RP2350B modules), SCK at most `max_mhz` (capped at the 135 MHz proven on
// hardware).  Returns the detected size in bytes, 0 if there is none.
uint32_t psram_init(unsigned cs_pin, int max_mhz);
uint32_t psram_size(void);
