#include "ui/creative_inventory.h"
#include "platform/platform.h"
#include "ui/block_icon.h"
#include "ui/font.h"
#include "ui/gui_atlas.h"
#include "world/block_system.h"
#include <stdio.h>

// Vanilla creative inventory layout in GUI units, relative to the window's
// top-left corner. The background texture is 256x256 with the window in its
// top-left 195x136.
enum {
  WINDOW_WIDTH = 195,
  WINDOW_HEIGHT = 136,
  BACKGROUND_TEXTURE_SIZE = 256,

  GRID_COLUMNS = 9,
  GRID_ROWS = 5,
  GRID_X = 9,
  GRID_Y = 18,
  SLOT_PITCH = 18,
  SLOT_SIZE = 16,
  HOTBAR_ROW_Y = 112,

  // The highlight sprite is 24x24 around a 16x16 slot.
  HIGHLIGHT_SIZE = 24,
  HIGHLIGHT_INSET = 4,

  SCROLL_TRACK_X = 175,
  SCROLL_TRACK_Y = 18,
  SCROLL_TRACK_HEIGHT = 112,
  SCROLLER_WIDTH = 12,
  SCROLLER_HEIGHT = 15,

  TAB_WIDTH = 26,
  TAB_HEIGHT = 32,
  TAB_PITCH = 27,
  TAB_OFFSET_Y = 28,
  TAB_ICON_X = 5,
  TAB_ICON_Y = 9,
  TAB_SPRITE_VARIANTS = 7,

  TITLE_X = 8,
  TITLE_Y = 6,

  // Vanilla tooltip: text offset from the pointer, padding around the text,
  // and the margin the 9-slice sprites draw outside the padded box.
  TOOLTIP_OFFSET_X = 12,
  TOOLTIP_OFFSET_Y = 12,
  TOOLTIP_PADDING = 3,
  TOOLTIP_SPRITE_MARGIN = 9,
  TOOLTIP_TEXT_HEIGHT = 8,
  TOOLTIP_FLIP_GAP = 16,

  CARRIED_ICON_HALF = 8,
  SPRITE_NAME_CAPACITY = 96,
  NO_ENTRY = -1
};

#define INVENTORY_DIM_COLOR ((Color8){16, 16, 16, 192})
#define TITLE_COLOR ((Color8){64, 64, 64, 255})

static const char *const TAB_NAMES[INVENTORY_TAB_COUNT] = {
    "Building Blocks", "Colored Blocks", "Natural Blocks"};

typedef struct {
  UiSpriteId Background;
  UiSpriteId Scroller;
  UiSpriteId ScrollerDisabled;
  UiSpriteId HighlightBack;
  UiSpriteId HighlightFront;
  UiSpriteId TooltipBackground;
  UiSpriteId TooltipFrame;
  UiSpriteId TabSelected[INVENTORY_TAB_COUNT];
  UiSpriteId TabUnselected[INVENTORY_TAB_COUNT];
} InventorySprites;

typedef struct {
  BlockID Blocks[BLOCK_REGISTRY_SIZE];
  int Count;
} TabCatalog;

typedef struct {
  bool Open;
  InventoryTab Tab;
  int ScrollRow;
  BlockID Carried;
  BlockID DesiredHotbar[HOTBAR_SIZE];
  bool HotbarDirty;
  bool DraggingScroller;
  int HoverOverride;
  InventorySprites Sprites;
  TabCatalog Catalogs[INVENTORY_TAB_COUNT];
} InventoryState;

static InventoryState *GetInventoryState(void) {
  static InventoryState State;
  return &State;
}

static InventoryTab TabForCategory(BlockCategory Category) {
  if (Category == BLOCK_CATEGORY_COLORED) {
    return INVENTORY_TAB_COLORED;
  }
  if (Category == BLOCK_CATEGORY_NATURAL) {
    return INVENTORY_TAB_NATURAL;
  }
  return INVENTORY_TAB_BUILDING;
}

static void BuildCatalogs(InventoryState *State) {
  for (int Tab = 0; Tab < INVENTORY_TAB_COUNT; Tab++) {
    State->Catalogs[Tab].Count = 0;
  }
  const BlockType *Registry = GetBlockRegistry();
  for (int Id = 1; Id < BLOCK_REGISTRY_SIZE; Id++) {
    const BlockType *Def = &Registry[Id];
    if (Def->Id != Id || Def->Category == BLOCK_CATEGORY_NONE) {
      continue;
    }
    TabCatalog *Catalog = &State->Catalogs[TabForCategory(Def->Category)];
    Catalog->Blocks[Catalog->Count++] = (BlockID)Id;
  }
}

static UiSpriteId RegisterTabSprite(const char *Kind, int Column) {
  char Name[SPRITE_NAME_CAPACITY];
  snprintf(Name, sizeof(Name), "container/creative_inventory/tab_top_%s_%d",
           Kind, (Column % TAB_SPRITE_VARIANTS) + 1);
  return GuiAtlasRegister(Name);
}

void CreativeInventoryInit(void) {
  InventoryState *State = GetInventoryState();
  InventorySprites *Sprites = &State->Sprites;
  Sprites->Background = GuiAtlasRegisterTexture(
      "gui/container/creative_inventory/tab_items.png");
  Sprites->Scroller =
      GuiAtlasRegister("container/creative_inventory/scroller");
  Sprites->ScrollerDisabled =
      GuiAtlasRegister("container/creative_inventory/scroller_disabled");
  Sprites->HighlightBack = GuiAtlasRegister("container/slot_highlight_back");
  Sprites->HighlightFront = GuiAtlasRegister("container/slot_highlight_front");
  Sprites->TooltipBackground = GuiAtlasRegister("tooltip/background");
  Sprites->TooltipFrame = GuiAtlasRegister("tooltip/frame");
  for (int Tab = 0; Tab < INVENTORY_TAB_COUNT; Tab++) {
    Sprites->TabSelected[Tab] = RegisterTabSprite("selected", Tab);
    Sprites->TabUnselected[Tab] = RegisterTabSprite("unselected", Tab);
  }
  State->HoverOverride = NO_ENTRY;
  BuildCatalogs(State);
}

// ---------------------------------------------------------------------------
// State and handlers
// ---------------------------------------------------------------------------

static int TabRowCount(const InventoryState *State) {
  int Count = State->Catalogs[State->Tab].Count;
  return (Count + GRID_COLUMNS - 1) / GRID_COLUMNS;
}

static int MaxScrollRow(const InventoryState *State) {
  int Rows = TabRowCount(State) - GRID_ROWS;
  return (Rows > 0) ? Rows : 0;
}

static void SetScrollRow(InventoryState *State, int Row) {
  int Max = MaxScrollRow(State);
  int Clamped = (Row > Max) ? Max : Row;
  State->ScrollRow = (Clamped < 0) ? 0 : Clamped;
}

// Block shown in a visible catalog entry, or air when the entry is past the
// end of the tab.
static BlockID EntryBlock(const InventoryState *State, int Entry) {
  const TabCatalog *Catalog = &State->Catalogs[State->Tab];
  int Index = (State->ScrollRow * GRID_COLUMNS) + Entry;
  if (Entry < 0 || Index >= Catalog->Count) {
    return BLOCK_ID_AIR;
  }
  return Catalog->Blocks[Index];
}

static void SetDesiredSlot(InventoryState *State, int Slot, BlockID Block) {
  if (Slot < 0 || Slot >= HOTBAR_SIZE) {
    return;
  }
  State->DesiredHotbar[Slot] = Block;
  State->HotbarDirty = true;
}

void CreativeInventoryOpen(const Player *PlayerVal) {
  InventoryState *State = GetInventoryState();
  State->Open = true;
  State->Carried = BLOCK_ID_AIR;
  State->DraggingScroller = false;
  for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
    State->DesiredHotbar[Slot] = PlayerVal->Hotbar[Slot];
  }
  SetScrollRow(State, State->ScrollRow);
  PlatformSetCursorDisabled(false);
}

void CreativeInventoryClose(void) {
  InventoryState *State = GetInventoryState();
  State->Open = false;
  State->Carried = BLOCK_ID_AIR;
  State->DraggingScroller = false;
  State->HoverOverride = NO_ENTRY;
  PlatformSetCursorDisabled(true);
}

bool CreativeInventoryIsOpen(void) { return GetInventoryState()->Open; }

bool CreativeInventoryTakeHotbarRequest(BlockID Out[HOTBAR_SIZE]) {
  InventoryState *State = GetInventoryState();
  if (!State->HotbarDirty) {
    return false;
  }
  for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
    Out[Slot] = State->DesiredHotbar[Slot];
  }
  State->HotbarDirty = false;
  return true;
}

void CreativeInventorySelectTab(InventoryTab Tab) {
  InventoryState *State = GetInventoryState();
  if (Tab < 0 || Tab >= INVENTORY_TAB_COUNT) {
    return;
  }
  State->Tab = Tab;
  State->ScrollRow = 0;
}

void CreativeInventoryScrollRows(int Rows) {
  InventoryState *State = GetInventoryState();
  SetScrollRow(State, State->ScrollRow + Rows);
}

bool CreativeInventoryPickEntry(int Entry) {
  InventoryState *State = GetInventoryState();
  BlockID Block = EntryBlock(State, Entry);
  if (Block == BLOCK_ID_AIR) {
    return false;
  }
  State->Carried = Block;
  return true;
}

bool CreativeInventoryPutCarried(int Slot) {
  InventoryState *State = GetInventoryState();
  if (State->Carried == BLOCK_ID_AIR || Slot < 0 || Slot >= HOTBAR_SIZE) {
    return false;
  }
  SetDesiredSlot(State, Slot, State->Carried);
  State->Carried = BLOCK_ID_AIR;
  return true;
}

int CreativeInventoryVisibleEntryCount(void) {
  const InventoryState *State = GetInventoryState();
  int Remaining = State->Catalogs[State->Tab].Count -
                  (State->ScrollRow * GRID_COLUMNS);
  int Visible = GRID_COLUMNS * GRID_ROWS;
  return (Remaining < Visible) ? Remaining : Visible;
}

void CreativeInventoryHoverEntry(int Entry) {
  GetInventoryState()->HoverOverride = (Entry >= 0) ? Entry : NO_ENTRY;
}

CreativeInventoryState CreativeInventoryGetState(void) {
  const InventoryState *State = GetInventoryState();
  return (CreativeInventoryState){.Open = State->Open,
                                  .Tab = State->Tab,
                                  .ScrollRow = State->ScrollRow,
                                  .MaxScrollRow = MaxScrollRow(State),
                                  .Carried = State->Carried};
}

const char *CreativeInventoryTabName(InventoryTab Tab) {
  static const char *const SCRIPT_NAMES[INVENTORY_TAB_COUNT] = {
      "building", "colored", "natural"};
  if (Tab < 0 || Tab >= INVENTORY_TAB_COUNT) {
    return "";
  }
  return SCRIPT_NAMES[Tab];
}

// ---------------------------------------------------------------------------
// Layout and hit testing
// ---------------------------------------------------------------------------

typedef struct {
  int X;
  int Y;
} InventoryOrigin;

static InventoryOrigin WindowOrigin(const UiContext *Ctx) {
  return (InventoryOrigin){(Ctx->GuiWidth - WINDOW_WIDTH) / 2,
                           (Ctx->GuiHeight - WINDOW_HEIGHT) / 2};
}

static bool PointerIn(const UiContext *Ctx, UiRect Rect) {
  float Px = Ctx->Pointer.X;
  float Py = Ctx->Pointer.Y;
  return Px >= (float)Rect.X && Py >= (float)Rect.Y &&
         Px < (float)(Rect.X + Rect.W) && Py < (float)(Rect.Y + Rect.H);
}

static UiRect GridSlotRect(InventoryOrigin Origin, int Entry) {
  return (UiRect){Origin.X + GRID_X + ((Entry % GRID_COLUMNS) * SLOT_PITCH),
                  Origin.Y + GRID_Y + ((Entry / GRID_COLUMNS) * SLOT_PITCH),
                  SLOT_SIZE, SLOT_SIZE};
}

static UiRect HotbarSlotRect(InventoryOrigin Origin, int Slot) {
  return (UiRect){Origin.X + GRID_X + (Slot * SLOT_PITCH),
                  Origin.Y + HOTBAR_ROW_Y, SLOT_SIZE, SLOT_SIZE};
}

static UiRect TabRect(InventoryOrigin Origin, int Tab) {
  return (UiRect){Origin.X + (Tab * TAB_PITCH), Origin.Y - TAB_OFFSET_Y,
                  TAB_WIDTH, TAB_HEIGHT};
}

static UiRect ScrollTrackRect(InventoryOrigin Origin) {
  return (UiRect){Origin.X + SCROLL_TRACK_X, Origin.Y + SCROLL_TRACK_Y,
                  SCROLLER_WIDTH, SCROLL_TRACK_HEIGHT};
}

static int HoveredGridEntry(const UiContext *Ctx, InventoryOrigin Origin) {
  int Visible = CreativeInventoryVisibleEntryCount();
  for (int Entry = 0; Entry < Visible; Entry++) {
    if (PointerIn(Ctx, GridSlotRect(Origin, Entry))) {
      return Entry;
    }
  }
  return NO_ENTRY;
}

static int HoveredHotbarSlot(const UiContext *Ctx, InventoryOrigin Origin) {
  for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
    if (PointerIn(Ctx, HotbarSlotRect(Origin, Slot))) {
      return Slot;
    }
  }
  return NO_ENTRY;
}

static int HoveredTab(const UiContext *Ctx, InventoryOrigin Origin) {
  for (int Tab = 0; Tab < INVENTORY_TAB_COUNT; Tab++) {
    if (PointerIn(Ctx, TabRect(Origin, Tab))) {
      return Tab;
    }
  }
  return NO_ENTRY;
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

// The scroller's centre follows the pointer along the track, so grabbing it
// anywhere moves it the way vanilla's does.
static void DragScroller(const UiContext *Ctx, InventoryState *State,
                         InventoryOrigin Origin) {
  int Max = MaxScrollRow(State);
  float Travel = (float)(SCROLL_TRACK_HEIGHT - SCROLLER_HEIGHT);
  float Top = (float)(Origin.Y + SCROLL_TRACK_Y) +
              ((float)SCROLLER_HEIGHT * UI_HALF);
  float Fraction = UiClamp01((Ctx->Pointer.Y - Top) / Travel);
  SetScrollRow(State, (int)((Fraction * (float)Max) + UI_HALF));
}

static void HandleHotbarClick(InventoryState *State, int Slot) {
  if (State->Carried != BLOCK_ID_AIR) {
    CreativeInventoryPutCarried(Slot);
    return;
  }
  if (State->DesiredHotbar[Slot] != BLOCK_ID_AIR) {
    State->Carried = State->DesiredHotbar[Slot];
    SetDesiredSlot(State, Slot, BLOCK_ID_AIR);
  }
}

static void HandlePress(const UiContext *Ctx, InventoryState *State,
                        InventoryOrigin Origin, const InventoryInput *Input) {
  int Tab = HoveredTab(Ctx, Origin);
  if (Tab != NO_ENTRY) {
    CreativeInventorySelectTab((InventoryTab)Tab);
    return;
  }
  int Entry = HoveredGridEntry(Ctx, Origin);
  if (Entry != NO_ENTRY) {
    if (Input != (void *)0 && Input->ShiftHeld) {
      BlockID Block = EntryBlock(State, Entry);
      if (Block != BLOCK_ID_AIR) {
        int VacantSlot = -1;
        for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
          if (State->DesiredHotbar[Slot] == BLOCK_ID_AIR) {
            VacantSlot = Slot;
            break;
          }
        }
        if (VacantSlot != -1) {
          SetDesiredSlot(State, VacantSlot, Block);
        } else {
          int TargetSlot = (Input->SelectedHotbarSlot >= 0 &&
                            Input->SelectedHotbarSlot < HOTBAR_SIZE)
                               ? Input->SelectedHotbarSlot
                               : 0;
          SetDesiredSlot(State, TargetSlot, Block);
        }
      }
      return;
    }
    CreativeInventoryPickEntry(Entry);
    return;
  }
  int Slot = HoveredHotbarSlot(Ctx, Origin);
  if (Slot != NO_ENTRY) {
    if (Input != (void *)0 && Input->ShiftHeld) {
      SetDesiredSlot(State, Slot, BLOCK_ID_AIR);
      return;
    }
    HandleHotbarClick(State, Slot);
    return;
  }
  if (PointerIn(Ctx, ScrollTrackRect(Origin))) {
    State->DraggingScroller = MaxScrollRow(State) > 0;
    return;
  }
  UiRect Window = {Origin.X, Origin.Y, WINDOW_WIDTH, WINDOW_HEIGHT};
  if (!PointerIn(Ctx, Window)) {
    State->Carried = BLOCK_ID_AIR;
  }
}

static void HandleInput(const UiContext *Ctx, InventoryState *State,
                        InventoryOrigin Origin, const InventoryInput *Input) {
  if (Ctx->Pointer.LeftPressed) {
    HandlePress(Ctx, State, Origin, Input);
  }
  if (State->DraggingScroller) {
    if (Ctx->Pointer.LeftDown) {
      DragScroller(Ctx, State, Origin);
    } else {
      State->DraggingScroller = false;
    }
  }
  if (Input->WheelDelta != 0) {
    CreativeInventoryScrollRows(-Input->WheelDelta);
  }
  if (Input->HotbarKey >= 0 && Input->HotbarKey < HOTBAR_SIZE) {
    int Entry = HoveredGridEntry(Ctx, Origin);
    int Slot = HoveredHotbarSlot(Ctx, Origin);
    if (Entry != NO_ENTRY) {
      BlockID Hovered = EntryBlock(State, Entry);
      if (Hovered != BLOCK_ID_AIR) {
        SetDesiredSlot(State, Input->HotbarKey, Hovered);
      }
    } else if (Slot != NO_ENTRY) {
      BlockID Current = State->DesiredHotbar[Slot];
      BlockID Target = State->DesiredHotbar[Input->HotbarKey];
      SetDesiredSlot(State, Input->HotbarKey, Current);
      SetDesiredSlot(State, Slot, Target);
    }
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void DrawTab(UiContext *Ctx, const InventoryState *State,
                    InventoryOrigin Origin, int Tab) {
  bool Selected = (Tab == (int)State->Tab);
  UiSpriteId Sprite = Selected ? State->Sprites.TabSelected[Tab]
                               : State->Sprites.TabUnselected[Tab];
  UiRect Rect = TabRect(Origin, Tab);
  UiSprite(Ctx, Sprite, Rect, COLOR_WHITE);
  if (State->Catalogs[Tab].Count > 0) {
    UiBlockIcon(Ctx, State->Catalogs[Tab].Blocks[0],
                (float)(Rect.X + TAB_ICON_X), (float)(Rect.Y + TAB_ICON_Y),
                (float)SLOT_SIZE);
  }
}

static void DrawBackground(UiContext *Ctx, const InventoryState *State,
                           InventoryOrigin Origin) {
  const GuiSprite *Info = GuiAtlasGetSprite(State->Sprites.Background);
  float UScale = (Info->U1 - Info->U0) / (float)BACKGROUND_TEXTURE_SIZE;
  float VScale = (Info->V1 - Info->V0) / (float)BACKGROUND_TEXTURE_SIZE;
  UiTexturedRect(Ctx, UI_TEXTURE_GUI_ATLAS, (float)Origin.X, (float)Origin.Y,
                 (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT, Info->U0, Info->V0,
                 Info->U0 + ((float)WINDOW_WIDTH * UScale),
                 Info->V0 + ((float)WINDOW_HEIGHT * VScale), COLOR_WHITE);
}

static void DrawSlotBlock(UiContext *Ctx, const InventoryState *State,
                          UiRect Slot, BlockID Block, bool Hovered) {
  UiRect Highlight = {Slot.X - HIGHLIGHT_INSET, Slot.Y - HIGHLIGHT_INSET,
                      HIGHLIGHT_SIZE, HIGHLIGHT_SIZE};
  if (Hovered) {
    UiNineSlice(Ctx, State->Sprites.HighlightBack, Highlight, COLOR_WHITE);
  }
  if (Block != BLOCK_ID_AIR) {
    UiBlockIcon(Ctx, Block, (float)Slot.X, (float)Slot.Y, (float)SLOT_SIZE);
  }
  if (Hovered) {
    UiNineSlice(Ctx, State->Sprites.HighlightFront, Highlight, COLOR_WHITE);
  }
}

static void DrawScroller(UiContext *Ctx, const InventoryState *State,
                         InventoryOrigin Origin) {
  int Max = MaxScrollRow(State);
  float Fraction = (Max > 0) ? ((float)State->ScrollRow / (float)Max) : 0.0F;
  int Travel = SCROLL_TRACK_HEIGHT - SCROLLER_HEIGHT;
  UiRect Rect = {Origin.X + SCROLL_TRACK_X,
                 Origin.Y + SCROLL_TRACK_Y + (int)(Fraction * (float)Travel),
                 SCROLLER_WIDTH, SCROLLER_HEIGHT};
  UiSprite(Ctx,
           (Max > 0) ? State->Sprites.Scroller
                     : State->Sprites.ScrollerDisabled,
           Rect, COLOR_WHITE);
}

// Kept inside the screen: flips to the pointer's left when it would run off
// the right edge, and is pushed down from the top edge.
static void DrawTooltip(UiContext *Ctx, const InventoryState *State,
                        const char *Text) {
  int TextWidth = (int)UiMeasureText(Text);
  int X = (int)Ctx->Pointer.X + TOOLTIP_OFFSET_X;
  int Y = (int)Ctx->Pointer.Y - TOOLTIP_OFFSET_Y;
  if (X + TextWidth + TOOLTIP_PADDING > Ctx->GuiWidth) {
    X = (int)Ctx->Pointer.X - TOOLTIP_FLIP_GAP - TextWidth;
  }
  if (Y - TOOLTIP_PADDING < 0) {
    Y = TOOLTIP_PADDING;
  }
  int Margin = TOOLTIP_PADDING + TOOLTIP_SPRITE_MARGIN;
  UiRect Box = {X - Margin, Y - Margin, TextWidth + (2 * Margin),
                TOOLTIP_TEXT_HEIGHT + (2 * Margin)};
  UiNineSlice(Ctx, State->Sprites.TooltipBackground, Box, COLOR_WHITE);
  UiNineSlice(Ctx, State->Sprites.TooltipFrame, Box, COLOR_WHITE);
  UiTextShadow(Ctx, Text, (float)X, (float)Y, COLOR_WHITE);
}

void DrawCreativeInventory(UiContext *Ctx, const InventoryInput *Input) {
  InventoryState *State = GetInventoryState();
  if (!State->Open) {
    return;
  }
  InventoryOrigin Origin = WindowOrigin(Ctx);
  if (State->HoverOverride != NO_ENTRY &&
      State->HoverOverride < CreativeInventoryVisibleEntryCount()) {
    UiRect Slot = GridSlotRect(Origin, State->HoverOverride);
    Ctx->Pointer.X = (float)Slot.X + ((float)Slot.W * UI_HALF);
    Ctx->Pointer.Y = (float)Slot.Y + ((float)Slot.H * UI_HALF);
  }
  HandleInput(Ctx, State, Origin, Input);

  UiFillRect(Ctx, (UiRect){0, 0, Ctx->GuiWidth, Ctx->GuiHeight},
             INVENTORY_DIM_COLOR);
  for (int Tab = 0; Tab < INVENTORY_TAB_COUNT; Tab++) {
    if (Tab != (int)State->Tab) {
      DrawTab(Ctx, State, Origin, Tab);
    }
  }
  DrawBackground(Ctx, State, Origin);
  DrawTab(Ctx, State, Origin, (int)State->Tab);
  UiText(Ctx, TAB_NAMES[State->Tab], (float)(Origin.X + TITLE_X),
         (float)(Origin.Y + TITLE_Y), TITLE_COLOR);

  int HoveredEntry = HoveredGridEntry(Ctx, Origin);
  int Visible = CreativeInventoryVisibleEntryCount();
  for (int Entry = 0; Entry < Visible; Entry++) {
    DrawSlotBlock(Ctx, State, GridSlotRect(Origin, Entry),
                  EntryBlock(State, Entry), Entry == HoveredEntry);
  }
  int HoveredSlot = HoveredHotbarSlot(Ctx, Origin);
  for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
    DrawSlotBlock(Ctx, State, HotbarSlotRect(Origin, Slot),
                  State->DesiredHotbar[Slot], Slot == HoveredSlot);
  }
  DrawScroller(Ctx, State, Origin);

  if (State->Carried != BLOCK_ID_AIR) {
    UiBlockIcon(Ctx, State->Carried, Ctx->Pointer.X - CARRIED_ICON_HALF,
                Ctx->Pointer.Y - CARRIED_ICON_HALF, (float)SLOT_SIZE);
    return;
  }

  BlockID Hovered = BLOCK_ID_AIR;
  if (HoveredEntry != NO_ENTRY) {
    Hovered = EntryBlock(State, HoveredEntry);
  } else if (HoveredSlot != NO_ENTRY) {
    Hovered = State->DesiredHotbar[HoveredSlot];
  }
  if (Hovered != BLOCK_ID_AIR) {
    DrawTooltip(Ctx, State, GetBlockDef(Hovered)->DisplayName);
  }
}
