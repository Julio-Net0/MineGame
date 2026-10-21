#ifndef UI_PAUSE_MENU_H
#define UI_PAUSE_MENU_H

#include "core/camera.h"
#include "ui/ui.h"
#include <stdbool.h>

// The ESC menu. It does not pause the world simulation; it only releases the
// cursor and takes input away from the player while open.

void PauseMenuToggle(void);
bool PauseMenuIsOpen(void);

// True once Save and Quit was chosen; the main loop exits through its normal
// shutdown path, which saves the world.
bool PauseMenuQuitRequested(void);

// Draws the menu when open. The FOV slider writes Camera->FovY directly.
void DrawPauseMenu(UiContext *Ctx, GameCamera *Camera);

#endif
