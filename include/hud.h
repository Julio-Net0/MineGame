#ifndef HUD_H
#define HUD_H

#include "player.h"
#include "world.h"
#include "raylib.h"

void DrawHUD(Player *Player, World *World, Camera3D Camera, bool ShowDebugF3);

#endif
