#pragma once
#include <stdint.h>

#define PSRAM_BASE 0x11000000u

// Bring up the QSPI PSRAM on XIP CS1 at `cs_pin` (8 on RP2350A carriers, 47 on
// RP2350B modules).  Returns the detected size in bytes, 0 if there is none.
uint32_t psram_init(unsigned cs_pin);
uint32_t psram_size(void);
