#include "render/hud.h"
#include "player/player.h"
#include "core/color.h"
#include "platform/platform.h"
#include "render/renderer.h"
#include "render/backend.h"
#include "world/world.h"
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
  DEBUG_TEXT_CAPACITY = 64
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

  #pragma unroll
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

  #pragma unroll 4
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

  RenderCamera RCam = {
      .Position = Camera.Position,
      .Target = Camera.Target,
      .Up = Camera.Up,
      .FovY = Camera.FovY,
  };

  int RenderedChunks = 0;
  #pragma unroll 4
  for (int Idx = 0; Idx < MAX_ACTIVE_CHUNKS; Idx++) {
    if (Idx >= TargetCount) {
      break;
    }
    if (WorldVal->Chunks[Idx].HasMesh &&
        IsChunkInFrustum(RCam, &WorldVal->Chunks[Idx])) {
      RenderedChunks++;
    }
  }
  snprintf(DebugText, sizeof(DebugText), "Rendered Chunks: %d / %d",
           RenderedChunks, MAX_ACTIVE_CHUNKS);
  RenderDrawText(DebugText, DEBUG_RENDERED_CHUNKS_X, DEBUG_RENDERED_CHUNKS_Y,
                 DEBUG_FONT_SIZE, COLOR_RAYWHITE);
}

void DrawHUD(Player *PlayerVal, World *WorldVal, GameCamera Camera, bool ShowDebugF3) {
  DrawHotbar(PlayerVal);
  if (ShowDebugF3) {
    DrawDebugScreen(PlayerVal, WorldVal, Camera);
  }
  DrawCrosshair();
}
