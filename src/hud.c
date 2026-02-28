#include "hud.h"
#include "block_system.h"
#include "player.h"
#include "raylib.h"

#define HOTBAR_SLOT_SIZE 50
#define HOTBAR_PADDING 5
#define HOTBAR_BOTTOM_MARGIN 20
#define HOTBAR_INNER_PADDING 10
#define HOTBAR_ICON_SIZE (HOTBAR_SLOT_SIZE - (HOTBAR_INNER_PADDING * 2))
#define HOTBAR_SELECTED_BOX_THICKNESS 3.0F
#define HOTBAR_TOTAL_WIDTH (((HOTBAR_SIZE * HOTBAR_SLOT_SIZE) + ((HOTBAR_SIZE - 1) * HOTBAR_PADDING)))
#define HOTBAR_TRANSPARENCY 0.5F

#define CROSSHAIR_SIZE 16
#define CROSSHAIR_THICKNESS 2
#define CROSSHAIR_TRANSPARENCY 0.8F

#define DEBUG_FONT_SIZE 20
#define DEBUG_FPS_X 10
#define DEBUG_FPS_Y 10
#define DEBUG_COORDS_X 10
#define DEBUG_COORDS_Y 30
#define DEBUG_ACTIVE_CHUNKS_X 10
#define DEBUG_ACTIVE_CHUNKS_Y 50


static void DrawHotbar(Player *player){
  int startX = (GetScreenWidth() - HOTBAR_TOTAL_WIDTH) / 2;
  int startY = GetScreenHeight() - HOTBAR_SLOT_SIZE - HOTBAR_BOTTOM_MARGIN;

  Color slotBgColor = Fade(BLACK, HOTBAR_TRANSPARENCY);

  for(int i = 0; i < HOTBAR_SIZE; i++){
    int x = startX + (i * (HOTBAR_SLOT_SIZE + HOTBAR_PADDING));

    DrawRectangle(x, startY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE, slotBgColor);

    unsigned char blockID = player->hotbar[i];
    if(blockID != 0){
      BlockType *def = GetBlockDef(blockID);
      DrawRectangle(x + HOTBAR_INNER_PADDING, startY + HOTBAR_INNER_PADDING, HOTBAR_ICON_SIZE, HOTBAR_ICON_SIZE, def->color);
    }

    if(i == player->selectedHotbarSlot){
      DrawRectangleLinesEx((Rectangle){(float)x, (float)startY, (float)HOTBAR_SLOT_SIZE, (float)HOTBAR_SLOT_SIZE}, HOTBAR_SELECTED_BOX_THICKNESS, WHITE);
    }else{
      DrawRectangleLines(x, startY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE, DARKGRAY);
    }
  }
}

static void DrawCrosshair(void){
  int centerX = GetScreenWidth() / 2;
  int centerY = GetScreenHeight() / 2;

  int halfSize = CROSSHAIR_SIZE / 2;
  int halfThick = CROSSHAIR_THICKNESS / 2;

  DrawRectangle(centerX - halfSize, centerY - halfThick, CROSSHAIR_SIZE, CROSSHAIR_THICKNESS, Fade(WHITE, CROSSHAIR_TRANSPARENCY));
  DrawRectangle(centerX - halfThick, centerY - halfSize, CROSSHAIR_THICKNESS, CROSSHAIR_SIZE, Fade(WHITE, CROSSHAIR_TRANSPARENCY));
}

static void DrawDebugScreen(Player *player, World *world){
  DrawFPS(DEBUG_FPS_X, DEBUG_FPS_Y);

  DrawText(TextFormat("XYZ: %.3f / %.5f / %.3f", player->position.x, player->position.y, player->position.z), DEBUG_COORDS_X, DEBUG_COORDS_Y, DEBUG_FONT_SIZE, RAYWHITE);
  DrawText(TextFormat("Active Chunks: %d / %d", world->chunkCount, MAX_ACTIVE_CHUNKS), DEBUG_ACTIVE_CHUNKS_X, DEBUG_ACTIVE_CHUNKS_Y, DEBUG_FONT_SIZE, RAYWHITE);
}

void DrawHUD(Player *player, World *world, bool showDebugF3){
  DrawHotbar(player);
  if(showDebugF3){
    DrawDebugScreen(player, world);
  }
  DrawCrosshair();
}
