#include "render/hud.h"
#include "player/player.h"
#include "core/color.h"
#include "platform/platform.h"
#include "render/renderer.h"
#include "render/backend.h"
#include "world/world.h"
#include "core/profiler.h"
#include <stdio.h>

enum {
  HOTBAR_SLOT_SIZE = 50,
  HOTBAR_PADDING = 5,
  HOTBAR_BOTTOM_MARGIN = 20,
  HOTBAR_INNER_PADDING = 10,
  HOTBAR_ICON_SIZE = (HOTBAR_SLOT_SIZE - (HOTBAR_INNER_PADDING * 2)),
  CROSSHAIR_SIZE = 16,
  CROSSHAIR_THICKNESS = 2,
  DEBUG_FONT_SIZE = 20,
  DEBUG_FPS_X = 10,
  DEBUG_FPS_Y = 10,
  DEBUG_COORDS_X = 10,
  DEBUG_COORDS_Y = 30,
  DEBUG_ACTIVE_CHUNKS_X = 10,
  DEBUG_ACTIVE_CHUNKS_Y = 50,
  DEBUG_RENDERED_CHUNKS_X = 10,
  DEBUG_RENDERED_CHUNKS_Y = 70,
  DEBUG_TEXT_CAPACITY = 96,
  DEBUG_TIMING_X = 10,
  DEBUG_TIMING_Y = 100,
  DEBUG_TIMING_LINE_HEIGHT = 20
};

#define HOTBAR_SELECTED_BOX_THICKNESS 3.0F
#define HOTBAR_TOTAL_WIDTH                                                     \
  (((HOTBAR_SIZE * HOTBAR_SLOT_SIZE) + ((HOTBAR_SIZE - 1) * HOTBAR_PADDING)))
#define HOTBAR_TRANSPARENCY 0.5F
#define CROSSHAIR_TRANSPARENCY 0.8F

static void DrawHotbar(Player *PlayerVal) {
  int StartX = (PlatformGetScreenWidth() - HOTBAR_TOTAL_WIDTH) / 2;
  int StartY = PlatformGetScreenHeight() - HOTBAR_SLOT_SIZE - HOTBAR_BOTTOM_MARGIN;

  Color8 SlotBgColor = Color8Alpha(COLOR_BLACK, HOTBAR_TRANSPARENCY);

  for (int Idx = 0; Idx < HOTBAR_SIZE; Idx++) {
    int XPos = StartX + (Idx * (HOTBAR_SLOT_SIZE + HOTBAR_PADDING));

    RenderDrawRect(XPos, StartY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE, SlotBgColor);

    unsigned char BlockId = PlayerVal->Hotbar[Idx];
    if (BlockId != 0) {
      RenderDrawBlockIcon(BlockId, HOTBAR_INNER_PADDING + XPos,
                    HOTBAR_INNER_PADDING + StartY, HOTBAR_ICON_SIZE);
    }

    if (Idx == PlayerVal->SelectedHotbarSlot) {
      RenderDrawRectLinesEx(XPos, StartY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE,
                            HOTBAR_SELECTED_BOX_THICKNESS, COLOR_WHITE);
    } else {
      RenderDrawRectLines(XPos, StartY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE,
                          COLOR_DARKGRAY);
    }
  }
}

static void DrawCrosshair(void) {
  int CenterX = PlatformGetScreenWidth() / 2;
  int CenterY = PlatformGetScreenHeight() / 2;

  int HalfSize = CROSSHAIR_SIZE / 2;
  int HalfThick = CROSSHAIR_THICKNESS / 2;

  Color8 CrosshairColor = Color8Alpha(COLOR_WHITE, CROSSHAIR_TRANSPARENCY);
  RenderDrawRect(CenterX - HalfSize, CenterY - HalfThick, CROSSHAIR_SIZE,
                 CROSSHAIR_THICKNESS, CrosshairColor);
  RenderDrawRect(CenterX - HalfThick, CenterY - HalfSize, CROSSHAIR_THICKNESS,
                 CROSSHAIR_SIZE, CrosshairColor);
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

static void DrawTimingPanel(void) {
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

  char Line[DEBUG_TEXT_CAPACITY];
  int YPos = DEBUG_TIMING_Y;

  snprintf(Line, sizeof(Line), "%-12s %8s %8s %8s %6s", "us/frame", "avg", "max",
           "1%low", "n");
  RenderDrawText(Line, DEBUG_TIMING_X, YPos, DEBUG_FONT_SIZE, COLOR_RAYWHITE);
  YPos += DEBUG_TIMING_LINE_HEIGHT;

  // The whole-frame total first, so the scopes below read as parts of it and
  // the remainder they do not account for is apparent rather than inferred.
  snprintf(Line, sizeof(Line), "%-12s %8u %8u %8u", "Frame",
           Panel->FrameStats.AverageUs, Panel->FrameStats.MaxUs,
           Panel->FrameStats.OnePercentLowUs);
  RenderDrawText(Line, DEBUG_TIMING_X, YPos, DEBUG_FONT_SIZE, COLOR_RAYWHITE);
  YPos += DEBUG_TIMING_LINE_HEIGHT;

  for (int Scope = 0; Scope < PROFILE_SCOPE_COUNT; Scope++) {
    const ProfileStats *Stats = &Panel->Stats[Scope];
    // A worker-side scope is summed across those threads, so it can legitimately
    // exceed the frame's own duration. Marked so that reading it as a share of
    // the frame is not the obvious interpretation. Asked of the profiler rather
    // than hardcoded: mesh building moved to the workers, and a hardcoded test
    // would have kept labelling it as main-thread time.
    const char *Suffix =
        IsProfileScopeWorkerSide((ProfileScope)Scope) ? " (worker)" : "";
    snprintf(Line, sizeof(Line), "%-12s %8u %8u %8u %6u%s",
             GetProfileScopeName((ProfileScope)Scope), Stats->AverageUs,
             Stats->MaxUs, Stats->OnePercentLowUs, Stats->AverageEntries,
             Suffix);
    RenderDrawText(Line, DEBUG_TIMING_X, YPos, DEBUG_FONT_SIZE, COLOR_RAYWHITE);
    YPos += DEBUG_TIMING_LINE_HEIGHT;
  }
}

#endif

static void DrawDebugScreen(Player *PlayerVal, World *WorldVal, GameCamera Camera) {
  char DebugText[DEBUG_TEXT_CAPACITY];

  int Fps = (int)(1.0F / PlatformGetFrameTime());
  snprintf(DebugText, sizeof(DebugText), "%d FPS", Fps);
  RenderDrawText(DebugText, DEBUG_FPS_X, DEBUG_FPS_Y, DEBUG_FONT_SIZE, COLOR_RAYWHITE);

  snprintf(DebugText, sizeof(DebugText), "XYZ: %.3f / %.5f / %.3f",
           PlayerVal->Position.x, PlayerVal->Position.y, PlayerVal->Position.z);
  RenderDrawText(DebugText, DEBUG_COORDS_X, DEBUG_COORDS_Y, DEBUG_FONT_SIZE,
                 COLOR_RAYWHITE);

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
  RenderDrawText(DebugText, DEBUG_ACTIVE_CHUNKS_X, DEBUG_ACTIVE_CHUNKS_Y,
                 DEBUG_FONT_SIZE, COLOR_RAYWHITE);

  // Taken from the draw loop rather than recomputed: it already counted, and
  // re-running the visibility test here was a second full scan over every chunk.
  snprintf(DebugText, sizeof(DebugText), "Rendered Chunks: %d / %d",
           GetLastRenderedChunkCount(), MAX_ACTIVE_CHUNKS);
  RenderDrawText(DebugText, DEBUG_RENDERED_CHUNKS_X, DEBUG_RENDERED_CHUNKS_Y,
                 DEBUG_FONT_SIZE, COLOR_RAYWHITE);

#ifdef MINEGAME_PROFILE
  DrawTimingPanel();
#endif
}

void DrawHUD(Player *PlayerVal, World *WorldVal, GameCamera Camera, bool ShowDebugF3) {
  DrawHotbar(PlayerVal);
  if (ShowDebugF3) {
    DrawDebugScreen(PlayerVal, WorldVal, Camera);
  }
  DrawCrosshair();
}
