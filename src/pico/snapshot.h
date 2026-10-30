// pico-amiga snapshot (.pas): the whole machine and what it ran on, in one file
// (pico-speccy's .pss, for the Amiga).  snapshot.cpp has the format.
#pragma once
#include <string>

#define SNAP_DIR      "/amiga/snapshots"
#define SNAP_SLOTS    40                       // Snapshots > Quick slots: persist1..40.pas
#define SNAP_NAME_LEN 64

namespace Snap {

// The quick slot's file, SNAP_DIR/persistN.pas.
std::string slotPath(int slot);
// Save the running machine (between frames: from the menu or a hot key).
bool save(const std::string& path, const std::string& name);
// Load one: the Kickstart it ran on is checked first (a refusal changes nothing
// and says why in `err`); then the model and ROM are switched, its disks put
// back, the state loaded, and the settings saved.  `warn`: disks that are gone
// or changed since.  A damaged file found only on the way leaves the Amiga reset.
bool load(const std::string& path, std::string& err, std::string& warn);
bool readName(const std::string& path, std::string& name);   // false: not a .pas
bool setName(const std::string& path, const std::string& name);
// The browser's F1: name, machine, Kickstart, disks, as lines of text.
void describe(const std::string& path, char* out, unsigned size);
// A name for a new snapshot: the disk set's, or DF0's disk's.
std::string defaultName();

} // namespace Snap
