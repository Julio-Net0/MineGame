#ifndef HUD_H
#define HUD_H

#include "player/player.h"
#include "world/world.h"
#include "core/camera.h"
#include "ui/ui.h"

// Registers the HUD's GUI sprites. Must run before the GUI atlas is built.
void InitHud(void);

void DrawHUD(UiContext *Ctx, Player *Player, World *World, GameCamera Camera,
             bool ShowDebugF3);

#endif
