#include "render/hud.h"
#include "player/player.h"
#include "raylib.h"
#include "render/renderer.h"
#include "world/world.h"

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
  DEBUG_RENDERED_CHUNKS_Y = 70
};

#define HOTBAR_SELECTED_BOX_THICKNESS 3.0F
#define HOTBAR_TOTAL_WIDTH                                                     \
  (((HOTBAR_SIZE * HOTBAR_SLOT_SIZE) + ((HOTBAR_SIZE - 1) * HOTBAR_PADDING)))
#define HOTBAR_TRANSPARENCY 0.5F
#define CROSSHAIR_TRANSPARENCY 0.8F

static void DrawHotbar(Player *PlayerVal) {
  int StartX = (GetScreenWidth() - HOTBAR_TOTAL_WIDTH) / 2;
  int StartY = GetScreenHeight() - HOTBAR_SLOT_SIZE - HOTBAR_BOTTOM_MARGIN;

  Color SlotBgColor = Fade(BLACK, HOTBAR_TRANSPARENCY);

  #pragma unroll
  for (int Idx = 0; Idx < HOTBAR_SIZE; Idx++) {
    int XPos = StartX + (Idx * (HOTBAR_SLOT_SIZE + HOTBAR_PADDING));

    DrawRectangle(XPos, StartY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE, SlotBgColor);

    unsigned char BlockId = PlayerVal->Hotbar[Idx];
    if (BlockId != 0) {
      DrawBlockIcon(BlockId, HOTBAR_INNER_PADDING + XPos,
                    HOTBAR_INNER_PADDING + StartY, HOTBAR_ICON_SIZE);
    }

    if (Idx == PlayerVal->SelectedHotbarSlot) {
      DrawRectangleLinesEx((Rectangle){(float)XPos, (float)StartY,
                                       (float)HOTBAR_SLOT_SIZE,
                                       (float)HOTBAR_SLOT_SIZE},
                           HOTBAR_SELECTED_BOX_THICKNESS, WHITE);
    } else {
      DrawRectangleLines(XPos, StartY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE,
                         DARKGRAY);
    }
  }
}

static void DrawCrosshair(void) {
  int CenterX = GetScreenWidth() / 2;
  int CenterY = GetScreenHeight() / 2;

  int HalfSize = CROSSHAIR_SIZE / 2;
  int HalfThick = CROSSHAIR_THICKNESS / 2;

  DrawRectangle(CenterX - HalfSize, CenterY - HalfThick, CROSSHAIR_SIZE,
                CROSSHAIR_THICKNESS, Fade(WHITE, CROSSHAIR_TRANSPARENCY));
  DrawRectangle(CenterX - HalfThick, CenterY - HalfSize, CROSSHAIR_THICKNESS,
                CROSSHAIR_SIZE, Fade(WHITE, CROSSHAIR_TRANSPARENCY));
}

static void DrawDebugScreen(Player *PlayerVal, World *WorldVal, Camera3D Camera) {
  DrawFPS(DEBUG_FPS_X, DEBUG_FPS_Y);

  DrawText(TextFormat("XYZ: %.3f / %.5f / %.3f", PlayerVal->Position.x,
                      PlayerVal->Position.y, PlayerVal->Position.z),
           DEBUG_COORDS_X, DEBUG_COORDS_Y, DEBUG_FONT_SIZE, RAYWHITE);

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
  DrawText(
      TextFormat("Active Chunks: %d / %d", ActiveChunks, MAX_ACTIVE_CHUNKS),
      DEBUG_ACTIVE_CHUNKS_X, DEBUG_ACTIVE_CHUNKS_Y, DEBUG_FONT_SIZE, RAYWHITE);

  int RenderedChunks = 0;
  #pragma unroll 4
  for (int Idx = 0; Idx < MAX_ACTIVE_CHUNKS; Idx++) {
    if (Idx >= TargetCount) {
      break;
    }
    if (WorldVal->Chunks[Idx].HasMesh &&
        IsChunkInFrustum(Camera, &WorldVal->Chunks[Idx])) {
      RenderedChunks++;
    }
  }
  DrawText(
      TextFormat("Rendered Chunks: %d / %d", RenderedChunks, MAX_ACTIVE_CHUNKS),
      DEBUG_RENDERED_CHUNKS_X, DEBUG_RENDERED_CHUNKS_Y, DEBUG_FONT_SIZE,
      RAYWHITE);
}

void DrawHUD(Player *PlayerVal, World *WorldVal, Camera3D Camera, bool ShowDebugF3) {
  DrawHotbar(PlayerVal);
  if (ShowDebugF3) {
    DrawDebugScreen(PlayerVal, WorldVal, Camera);
  }
  DrawCrosshair();
}
