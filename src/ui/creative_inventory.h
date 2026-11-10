#ifndef UI_CREATIVE_INVENTORY_H
#define UI_CREATIVE_INVENTORY_H

#include "player/player.h"
#include "ui/ui.h"
#include "world/block_id.h"
#include <stdbool.h>

// The creative inventory screen. It never writes the player's hotbar: edits go
// to a client-side desired hotbar, which the caller takes as a request and
// sends through the player intent, so the simulation (local or server)
// validates and applies it.

typedef enum {
  INVENTORY_TAB_BUILDING = 0,
  INVENTORY_TAB_COLORED,
  INVENTORY_TAB_NATURAL,
  INVENTORY_TAB_COUNT
} InventoryTab;

// Per-frame input the UI pointer does not carry.
typedef struct {
  int WheelDelta; // positive scrolls the catalog up
  int HotbarKey;  // -1, or the 0-based slot whose number key was pressed
} InventoryInput;

// Registers the screen's sprites and builds the per-tab block lists. Must run
// after block definitions are loaded and before the GUI atlas is built.
void CreativeInventoryInit(void);

void CreativeInventoryOpen(const Player *PlayerVal);
void CreativeInventoryClose(void);
bool CreativeInventoryIsOpen(void);

// Draws the screen and applies this frame's pointer and key input to it. Does
// nothing while closed.
void DrawCreativeInventory(UiContext *Ctx, const InventoryInput *Input);

// True when the desired hotbar changed since the last call; copies it to Out.
bool CreativeInventoryTakeHotbarRequest(BlockID Out[HOTBAR_SIZE]);

// Handlers shared by mouse input and scripts. Entry indices count the visible
// catalog entries from 0 (the top-left slot at the current scroll position).
void CreativeInventorySelectTab(InventoryTab Tab);
void CreativeInventoryScrollRows(int Rows);
bool CreativeInventoryPickEntry(int Entry);
bool CreativeInventoryPutCarried(int Slot);
int CreativeInventoryVisibleEntryCount(void);

// Places a synthetic pointer over a visible catalog entry for scripted runs,
// where the real mouse is ignored, so the highlight and tooltip can be
// captured. A negative entry removes it.
void CreativeInventoryHoverEntry(int Entry);

typedef struct {
  bool Open;
  InventoryTab Tab;
  int ScrollRow;
  int MaxScrollRow;
  BlockID Carried;
} CreativeInventoryState;

CreativeInventoryState CreativeInventoryGetState(void);
const char *CreativeInventoryTabName(InventoryTab Tab);

#endif
