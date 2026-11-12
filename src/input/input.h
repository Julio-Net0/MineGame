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
  bool CommandOpen;
  bool PauseToggle;
  bool InventoryToggle;
  // -1, or the 0-based hotbar slot whose number key was pressed this frame.
  // Reported regardless of control so an open screen can use it.
  int HotbarKey;
} SystemInput;

// Per-frame text-entry query for a text field: the printable characters typed
// this frame as UTF-8 bytes (Count is a byte count and never splits a
// sequence) plus the edit-key edges. Backspace includes key-repeat; Submit is
// enter, Cancel is escape or delete. The caller consumes intent, never key codes.
typedef struct {
  char Chars[CHAT_MAX_INPUT_CHARS];
  int Count;
  bool Backspace;
  bool Delete;
  bool Submit;
  bool Cancel;
  bool MoveLeft;
  bool MoveRight;
  bool MoveHome;
  bool MoveEnd;
  bool WordStep;
  bool ShiftHeld;
  bool SelectAll;
  bool Copy;
  bool Cut;
  bool Paste;
  bool Undo;
  bool HistoryPrev;
  bool HistoryNext;
  bool Autocomplete;
} TextInput;

// Per-frame pointer state for UI widgets: cursor position in window pixels and
// each button's press edge, held state and release edge.
typedef struct {
  Vec2 Cursor;
  bool LeftPressed;
  bool LeftDown;
  bool LeftReleased;
  bool RightPressed;
  bool RightDown;
  bool RightReleased;
} PointerInput;

// Client input layer: polls the input library into an abstract PlayerInput,
// reports system-toggle edges and the mouse-look delta, and builds a PlayerView
// from the client camera. In multiplayer, a network layer would produce these
// instead. Implemented by a single swappable input TU (input/glfw_input.c).
PlayerInput PollPlayerInput(bool HasControl);
SystemInput PollSystemInput(void);
TextInput PollTextInput(void);
PointerInput PollPointerInput(void);
int InputGetScrollDelta(void);
Vec2 InputGetLookDelta(void);
bool InputIsShiftDown(void);
PlayerView PlayerViewFromCamera(GameCamera Camera);

#endif
