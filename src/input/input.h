#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include "core/camera.h"
#include "core/vecmath.h"
#include "player/player.h"

// System-toggle edges surfaced as intent, decoupled from key codes.
typedef struct {
  bool FullscreenToggle;
  bool DebugToggle;
} SystemInput;

// Client input layer: polls the input library into an abstract PlayerInput,
// reports system-toggle edges and the mouse-look delta, and builds a PlayerView
// from the client camera. In multiplayer, a network layer would produce these
// instead. Implemented by a single swappable input TU (input/raylib_input.c).
PlayerInput PollPlayerInput(bool HasControl);
SystemInput PollSystemInput(void);
Vec2 InputGetLookDelta(void);
PlayerView PlayerViewFromCamera(GameCamera Camera);

#endif
