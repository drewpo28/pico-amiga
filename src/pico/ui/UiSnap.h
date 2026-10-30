// pico-amiga — Snapshots: the menu's level and the hot keys (UiSnap.cpp).
#pragma once
#include <stdint.h>
#include <string>
#include "UiModel.h"

namespace nm {

// Snapshots > Quick slots: the 40 slots as one K_PICK list (UiTree.cpp).
extern Option persist_foot[1];
const Option* persist_rows(uint8_t& cnt);
void          persist_key(int32_t slot, uint8_t key);
const char*   persist_vlabel();
// What Enter does in the list this session (Win+F4 opened it: save), and the
// session's slot rows (freed when the menu closes).
void persistEnterVerb(bool save);
void snapSessionBegin();
void snapSessionEnd();

// Snapshots > Load from file: the browser on SNAP_DIR.
void loadSnapshotFile();
// A .pas from the card into the machine (Load from file, Win+F5's Enter); false,
// with a toast, when it did not load.  Inside a gfx session.
bool snapLoadPath(const std::string& path);

// The quick slots' row in the tree, for the hot keys (UiTree.cpp).
const Node* persistNodeFor();

} // namespace nm
