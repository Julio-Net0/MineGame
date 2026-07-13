#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include "core/camera.h"
#include "core/vecmath.h"
#include "player/player.h"
#include "ui/chat.h"

// System-toggle edges surfaced as intent, decoupled from key codes.
typedef struct {
  bool FullscreenToggle;
  bool DebugToggle;
  bool ChatOpen;
} SystemInput;

// Per-frame text-entry query for a text field: the printable characters typed
// this frame plus the edit-key edges. Backspace includes key-repeat; Submit is
// enter, Cancel is delete. The caller consumes intent, never key codes.
typedef struct {
  char Chars[CHAT_MAX_INPUT_CHARS];
  int Count;
  bool Backspace;
  bool Submit;
  bool Cancel;
} TextInput;

// Client input layer: polls the input library into an abstract PlayerInput,
// reports system-toggle edges and the mouse-look delta, and builds a PlayerView
// from the client camera. In multiplayer, a network layer would produce these
// instead. Implemented by a single swappable input TU (input/raylib_input.c).
PlayerInput PollPlayerInput(bool HasControl);
SystemInput PollSystemInput(void);
TextInput PollTextInput(void);
int InputGetScrollDelta(void);
Vec2 InputGetLookDelta(void);
PlayerView PlayerViewFromCamera(GameCamera Camera);

#endif
