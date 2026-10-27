// pico-amiga (from pico-speccy) — the fullscreen file browser.
//
// Returns "R" + name when a file was chosen (name relative to fdir, which tracks
// navigation), "" when cancelled. Uses the /tmp .idx cache (SortedFiles.h) and
// remembers the position per type, so a browse position survives within a session.

#pragma once


#include <string>
#include <stdint.h>

namespace nm {

// What is being picked: which extensions show bright, and where the cursor was last.
// BROWSE_CFG is housekeeping only (Enter views text, nothing is picked).
enum BrowseType : uint8_t { BROWSE_ALL = 0, BROWSE_ADF, BROWSE_ROM, BROWSE_CFG, BROWSE_COUNT };

// `root`, when given, is the highest directory this browse may go up to (it must be
// a prefix of fdir and end with '/').
std::string browseFile(std::string& fdir, const std::string& title, uint8_t ftype,
                       const char* root = nullptr);

} // namespace nm
