#include "ui/hud.h"
#include "player/player.h"
#include "core/color.h"
#include "platform/platform.h"
#include "render/renderer.h"
#include "render/backend.h"
#include "ui/block_icon.h"
#include "ui/font.h"
#include "world/world.h"
#include "core/profiler.h"
#include <stdio.h>

// Hotbar and crosshair sizes are vanilla's, in GUI units: the sprites are
// authored at exactly these sizes, and a higher-resolution resource pack keeps
// them because the size comes from here rather than from the image.
enum {
  HOTBAR_WIDTH = 182,
  HOTBAR_HEIGHT = 22,
  HOTBAR_SLOT_PITCH = 20,
  HOTBAR_SELECTION_WIDTH = 24,
  HOTBAR_SELECTION_HEIGHT = 23,
  HOTBAR_ICON_INSET = 3,
  HOTBAR_ICON_SIZE = 16,
  CROSSHAIR_SIZE = 15,
  DEBUG_TEXT_CAPACITY = 96,
  // Vanilla F3 layout: lines start two units in from the corner, one font line
  // apart, each on its own translucent grey strip.
  DEBUG_MARGIN = 2,
  DEBUG_STRIP_PAD = 1,
  // Timing table columns. The font is proportional, so columns are placed
  // explicitly and numbers right-aligned instead of padded with spaces.
  DEBUG_NAME_COLUMN = 64,
  DEBUG_NUMBER_COLUMN = 34,
  DEBUG_NUMBER_COLUMNS = 4,
  DEBUG_SUFFIX_GAP = 4
};

typedef struct {
  UiSpriteId Hotbar;
  UiSpriteId HotbarSelection;
  UiSpriteId Crosshair;
} HudSprites;

static HudSprites *GetHudSprites(void) {
  static HudSprites Sprites;
  return &Sprites;
}

void InitHud(void) {
  HudSprites *Sprites = GetHudSprites();
  Sprites->Hotbar = GuiAtlasRegister("hud/hotbar");
  Sprites->HotbarSelection = GuiAtlasRegister("hud/hotbar_selection");
  Sprites->Crosshair = GuiAtlasRegister("hud/crosshair");
}

static void DrawHotbar(UiContext *Ctx, Player *PlayerVal) {
  HudSprites *Sprites = GetHudSprites();
  int StartX = (Ctx->GuiWidth - HOTBAR_WIDTH) / 2;
  int StartY = Ctx->GuiHeight - HOTBAR_HEIGHT;

  UiSprite(Ctx, Sprites->Hotbar,
           (UiRect){StartX, StartY, HOTBAR_WIDTH, HOTBAR_HEIGHT}, COLOR_WHITE);

  // Offset one unit up and left: the selection frame is two units larger than
  // a slot so it overlaps the hotbar's own border.
  int Selected = PlayerVal->SelectedHotbarSlot;
  UiSprite(Ctx, Sprites->HotbarSelection,
           (UiRect){StartX - 1 + (Selected * HOTBAR_SLOT_PITCH), StartY - 1,
                    HOTBAR_SELECTION_WIDTH, HOTBAR_SELECTION_HEIGHT},
           COLOR_WHITE);

  for (int Idx = 0; Idx < HOTBAR_SIZE; Idx++) {
    unsigned char BlockId = PlayerVal->Hotbar[Idx];
    if (BlockId != 0) {
      float IconX =
          (float)(StartX + HOTBAR_ICON_INSET + (Idx * HOTBAR_SLOT_PITCH));
      float IconY = (float)(StartY + HOTBAR_ICON_INSET);
      UiBlockIcon(Ctx, BlockId, IconX, IconY, (float)HOTBAR_ICON_SIZE);
    }
  }
}

static void DrawCrosshair(UiContext *Ctx) {
  int X = (Ctx->GuiWidth - CROSSHAIR_SIZE) / 2;
  int Y = (Ctx->GuiHeight - CROSSHAIR_SIZE) / 2;
  UiSprite(Ctx, GetHudSprites()->Crosshair,
           (UiRect){X, Y, CROSSHAIR_SIZE, CROSSHAIR_SIZE}, COLOR_WHITE);
}

#define DEBUG_TEXT_COLOR ((Color8){224, 224, 224, 255})
#define DEBUG_STRIP_COLOR ((Color8){80, 80, 80, 144})

static void DrawDebugStrip(UiContext *Ctx, int X, int Y, float Width) {
  UiFillRect(Ctx,
             (UiRect){X - DEBUG_STRIP_PAD, Y - DEBUG_STRIP_PAD,
                      (int)Width + (DEBUG_STRIP_PAD * 2), FONT_LINE_HEIGHT},
             DEBUG_STRIP_COLOR);
}

// One overlay line on its strip; returns the Y of the next line.
static int DrawDebugLine(UiContext *Ctx, const char *Text, int Y) {
  DrawDebugStrip(Ctx, DEBUG_MARGIN, Y, UiMeasureText(Text));
  UiText(Ctx, Text, (float)DEBUG_MARGIN, (float)Y, DEBUG_TEXT_COLOR);
  return Y + FONT_LINE_HEIGHT;
}

#ifdef MINEGAME_PROFILE

// Per-subsystem cost in microseconds, not milliseconds: on a fast machine the
// whole frame costs a fraction of a millisecond and every subsystem would read
// as zero. Average alongside peak and 1% low, because the stall this panel
// exists to diagnose happens on a minority of frames and an average hides it
// exactly when it matters.
//
// Recomputed a few times a second rather than every frame. Scanning the window
// per frame would waste work on a number nobody can read at 60 Hz, and a
// readout that changes 60 times a second is unreadable anyway.
#define TIMING_REFRESH_INTERVAL 0.25

typedef struct {
  ProfileStats Stats[PROFILE_SCOPE_COUNT];
  ProfileStats FrameStats;
  double LastRefreshTime;
  bool HasSamples;
} TimingPanelState;

static TimingPanelState *GetTimingPanelState(void) {
  static TimingPanelState State;
  return &State;
}

// Name, up to four right-aligned numeric cells and a suffix, on one strip.
static int DrawTimingRow(UiContext *Ctx, int Y, const char *Name,
                         const char *const Cells[DEBUG_NUMBER_COLUMNS],
                         const char *Suffix) {
  float Width = (float)(DEBUG_NAME_COLUMN +
                        (DEBUG_NUMBER_COLUMNS * DEBUG_NUMBER_COLUMN));
  float SuffixWidth = UiMeasureText(Suffix);
  if (SuffixWidth > 0.0F) {
    Width += (float)DEBUG_SUFFIX_GAP + SuffixWidth;
  }
  DrawDebugStrip(Ctx, DEBUG_MARGIN, Y, Width);

  UiText(Ctx, Name, (float)DEBUG_MARGIN, (float)Y, DEBUG_TEXT_COLOR);
  float Right = (float)(DEBUG_MARGIN + DEBUG_NAME_COLUMN);
  for (int Col = 0; Col < DEBUG_NUMBER_COLUMNS; Col++) {
    Right += (float)DEBUG_NUMBER_COLUMN;
    if (Cells[Col] != NULL) {
      UiText(Ctx, Cells[Col], Right - UiMeasureText(Cells[Col]), (float)Y,
             DEBUG_TEXT_COLOR);
    }
  }
  if (SuffixWidth > 0.0F) {
    UiText(Ctx, Suffix, Right + (float)DEBUG_SUFFIX_GAP, (float)Y,
           DEBUG_TEXT_COLOR);
  }
  return Y + FONT_LINE_HEIGHT;
}

static int DrawTimingPanel(UiContext *Ctx, int Y) {
  TimingPanelState *Panel = GetTimingPanelState();

  double Now = PlatformGetTime();
  if (!Panel->HasSamples || (Now - Panel->LastRefreshTime) >= TIMING_REFRESH_INTERVAL) {
    for (int Scope = 0; Scope < PROFILE_SCOPE_COUNT; Scope++) {
      ProfilerQuery((ProfileScope)Scope, &Panel->Stats[Scope]);
    }
    ProfilerQueryFrame(&Panel->FrameStats);
    Panel->LastRefreshTime = Now;
    Panel->HasSamples = true;
  }

  const char *const HEADER_CELLS[DEBUG_NUMBER_COLUMNS] = {"avg", "max", "1%low", "n"};
  Y = DrawTimingRow(Ctx, Y, "us/frame", HEADER_CELLS, "");

  enum { NUMBER_TEXT_SIZE = 16 };
  char Avg[NUMBER_TEXT_SIZE];
  char Max[NUMBER_TEXT_SIZE];
  char Low[NUMBER_TEXT_SIZE];
  char Count[NUMBER_TEXT_SIZE];

  // The whole-frame total first, so the scopes below read as parts of it and
  // the remainder they do not account for is apparent rather than inferred.
  snprintf(Avg, sizeof(Avg), "%u", Panel->FrameStats.AverageUs);
  snprintf(Max, sizeof(Max), "%u", Panel->FrameStats.MaxUs);
  snprintf(Low, sizeof(Low), "%u", Panel->FrameStats.OnePercentLowUs);
  const char *const FRAME_CELLS[DEBUG_NUMBER_COLUMNS] = {Avg, Max, Low, NULL};
  Y = DrawTimingRow(Ctx, Y, "Frame", FRAME_CELLS, "");

  for (int Scope = 0; Scope < PROFILE_SCOPE_COUNT; Scope++) {
    const ProfileStats *Stats = &Panel->Stats[Scope];
    // A worker-side scope is summed across those threads, so it can legitimately
    // exceed the frame's own duration. Marked so that reading it as a share of
    // the frame is not the obvious interpretation. Asked of the profiler rather
    // than hardcoded: mesh building moved to the workers, and a hardcoded test
    // would have kept labelling it as main-thread time.
    const char *Suffix =
        IsProfileScopeWorkerSide((ProfileScope)Scope) ? "(worker)" : "";
    snprintf(Avg, sizeof(Avg), "%u", Stats->AverageUs);
    snprintf(Max, sizeof(Max), "%u", Stats->MaxUs);
    snprintf(Low, sizeof(Low), "%u", Stats->OnePercentLowUs);
    snprintf(Count, sizeof(Count), "%u", Stats->AverageEntries);
    const char *const ROW_CELLS[DEBUG_NUMBER_COLUMNS] = {Avg, Max, Low, Count};
    Y = DrawTimingRow(Ctx, Y, GetProfileScopeName((ProfileScope)Scope),
                      ROW_CELLS, Suffix);
  }
  return Y;
}

#endif

static void DrawDebugScreen(UiContext *Ctx, Player *PlayerVal, World *WorldVal) {
  char DebugText[DEBUG_TEXT_CAPACITY];
  int Y = DEBUG_MARGIN;

  int Fps = (int)(1.0F / PlatformGetFrameTime());
  snprintf(DebugText, sizeof(DebugText), "%d FPS", Fps);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  snprintf(DebugText, sizeof(DebugText), "XYZ: %.3f / %.5f / %.3f",
           PlayerVal->Position.x, PlayerVal->Position.y, PlayerVal->Position.z);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  int ActiveChunks = 0;
  int WorldChunkCount = WorldVal->ChunkCount;
  int TargetCount = WorldChunkCount < MAX_ACTIVE_CHUNKS ? WorldChunkCount : MAX_ACTIVE_CHUNKS;

  for (int Idx = 0; Idx < MAX_ACTIVE_CHUNKS; Idx++) {
    if (Idx >= TargetCount) {
      break;
    }
    if (WorldVal->Chunks[Idx].IsGenerated) {
      ActiveChunks++;
    }
  }
  snprintf(DebugText, sizeof(DebugText), "Active Chunks: %d / %d", ActiveChunks,
           MAX_ACTIVE_CHUNKS);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  // Taken from the draw loop rather than recomputed: it already counted, and
  // re-running the visibility test here was a second full scan over every chunk.
  snprintf(DebugText, sizeof(DebugText), "Rendered Chunks: %d / %d",
           GetLastRenderedChunkCount(), MAX_ACTIVE_CHUNKS);
  Y = DrawDebugLine(Ctx, DebugText, Y);

#ifdef MINEGAME_PROFILE
  DrawTimingPanel(Ctx, Y + FONT_LINE_HEIGHT);
#else
  (void)Y;
#endif
}

void DrawHUD(UiContext *Ctx, Player *PlayerVal, World *WorldVal,
             GameCamera Camera, bool ShowDebugF3) {
  (void)Camera;
  DrawHotbar(Ctx, PlayerVal);
  if (ShowDebugF3) {
    DrawDebugScreen(Ctx, PlayerVal, WorldVal);
  }
  DrawCrosshair(Ctx);
}
