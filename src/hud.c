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
#define HOTBAR_TOTAL_WIDTH ((HOTBAR_SIZE * HOTBAR_SLOT_SIZE) + ((HOTBAR_SIZE - 1) * HOTBAR_PADDING))
#define HOTBAR_TRANSPARENCY 0.5F

#define CROSSHAIR_SIZE 16
#define CROSSHAIR_THICKNESS 2
#define CROSSHAIR_TRANSPARENCY 0.8F

static void DrawHotbar(Player *player){

  int startX = (GetScreenWidth() - HOTBAR_TOTAL_WIDTH) / 2;
  int startY = GetScreenHeight() - HOTBAR_SLOT_SIZE - HOTBAR_BOTTOM_MARGIN;

  for(int i = 0; i < HOTBAR_SIZE; i++){
    int x = startX + (i * (HOTBAR_SLOT_SIZE + HOTBAR_PADDING));

    DrawRectangle(x, startY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE, Fade(BLACK, HOTBAR_TRANSPARENCY));

    unsigned char blockID = player->hotbar[i];
    if(blockID != 0){
      BlockType *def = GetBlockDef(blockID);
      DrawRectangle(x + HOTBAR_INNER_PADDING, startY + HOTBAR_INNER_PADDING, HOTBAR_ICON_SIZE, HOTBAR_ICON_SIZE, def->color);
    }

    if(i == player->selectedHotbarSlot){
      DrawRectangleLinesEx((Rectangle){x, startY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE}, HOTBAR_SELECTED_BOX_THICKNESS, WHITE);
    }else{
      DrawRectangleLines(x, startY, HOTBAR_SLOT_SIZE, HOTBAR_SLOT_SIZE, DARKGRAY);
    }
  }
}

static void DrawCrosshair(){
  
  int centerX = GetScreenWidth() / 2;
  int centerY = GetScreenHeight() / 2;

  int halfSize = CROSSHAIR_SIZE / 2;
  int halfThick = CROSSHAIR_THICKNESS / 2;

  DrawRectangle(centerX - halfSize, centerY - halfThick, CROSSHAIR_SIZE, CROSSHAIR_THICKNESS, Fade(WHITE, CROSSHAIR_TRANSPARENCY));
  DrawRectangle(centerX - halfThick, centerY - halfSize, CROSSHAIR_THICKNESS, CROSSHAIR_SIZE, Fade(WHITE, CROSSHAIR_TRANSPARENCY));
}

void DrawHUD(Player *player, World *world, bool showDebugF3){
  DrawHotbar(player);
  DrawCrosshair();
}
