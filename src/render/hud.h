#ifndef HUD_H
#define HUD_H

#include "player/player.h"
#include "world/world.h"
#include "core/camera.h"

void DrawHUD(Player *Player, World *World, GameCamera Camera, bool ShowDebugF3);

#endif
