// PICO-AMIGA: host stand-in; an RP2350B (package_sel 0).
#pragma once
#include "pico_host.h"
typedef struct { uint32_t chip_id, package_sel, platform, gitref_rp2350; } sysinfo_hw_t;
#define sysinfo_hw ((sysinfo_hw_t *)host_sysinfo)
