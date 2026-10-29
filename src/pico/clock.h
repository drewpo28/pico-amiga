#pragma once
// Set the core voltage (`vreg_mv`, Options > Overclock > Core voltage) and switch
// clk_sys to `mhz` (252/378/504 are the tested values). Returns the clock actually
// applied (252 if `mhz` could not be reached). The flash runs at the fastest
// SCK = clk_sys / n that does not exceed `flash_mhz`.
int clock_overclock(int mhz, int flash_mhz, int vreg_mv);
