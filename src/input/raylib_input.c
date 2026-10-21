#include "input/input.h"
#include "render/rl_compat.h"
#include "core/vecmath.h"
#include "raylib.h"
#include <string.h>

enum {
  DELETE_CODEPOINT = 127
};

PlayerInput PollPlayerInput(bool HasControl) {
  PlayerInput Input = {0};
  Input.HotbarSelect = -1;

  if (!HasControl) {
    return Input;
  }

  if (IsKeyDown(KEY_W)) { Input.MoveForward += 1.0F; }
  if (IsKeyDown(KEY_S)) { Input.MoveForward -= 1.0F; }
  if (IsKeyDown(KEY_A)) { Input.MoveX -= 1.0F; }
  if (IsKeyDown(KEY_D)) { Input.MoveX += 1.0F; }

  Input.Jump = IsKeyPressed(KEY_SPACE);
  Input.AscendHeld = IsKeyDown(KEY_SPACE);
  Input.DescendHeld = IsKeyDown(KEY_LEFT_CONTROL);

  for (int IdxI = 0; IdxI < HOTBAR_SIZE; IdxI++) {
    if (IsKeyPressed(KEY_ONE + IdxI)) {
      Input.HotbarSelect = IdxI;
    }
  }

  Input.HotbarScroll = (int)GetMouseWheelMove();

  Input.Break = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
  Input.Place = IsMouseButtonPressed(MOUSE_RIGHT_BUTTON);

  return Input;
}

SystemInput PollSystemInput(void) {
  SystemInput System = {0};
  System.FullscreenToggle = IsKeyPressed(KEY_F11);
  System.DebugToggle = IsKeyPressed(KEY_F3);
  System.ChatOpen = IsKeyPressed(KEY_T);
  System.PauseToggle = IsKeyPressed(KEY_ESCAPE);
  return System;
}

TextInput PollTextInput(void) {
  TextInput Text = {0};

  int Key = GetCharPressed();
  for (int LoopIdx = 0; LoopIdx < CHAT_MAX_INPUT_CHARS; LoopIdx++) {
    if (Key <= 0) {
      break;
    }
    // Any printable codepoint, encoded as UTF-8, so accented letters typed
    // on the keyboard reach the chat. Only whole sequences are appended.
    if (Key >= ' ' && Key != DELETE_CODEPOINT) {
      int Size = 0;
      const char *Encoded = CodepointToUTF8(Key, &Size);
      if (Size > 0 && Text.Count + Size <= CHAT_MAX_INPUT_CHARS) {
        memcpy(&Text.Chars[Text.Count], Encoded, (size_t)Size);
        Text.Count += Size;
      }
    }
    Key = GetCharPressed();
  }

  Text.Backspace = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
  Text.Submit = IsKeyPressed(KEY_ENTER);
  Text.Cancel = IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_DELETE);
  return Text;
}

PointerInput PollPointerInput(void) {
  PointerInput Pointer = {0};
  Pointer.Cursor = Vec2FromRL(GetMousePosition());
  Pointer.LeftPressed = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
  Pointer.LeftDown = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
  Pointer.LeftReleased = IsMouseButtonReleased(MOUSE_LEFT_BUTTON);
  Pointer.RightPressed = IsMouseButtonPressed(MOUSE_RIGHT_BUTTON);
  Pointer.RightDown = IsMouseButtonDown(MOUSE_RIGHT_BUTTON);
  Pointer.RightReleased = IsMouseButtonReleased(MOUSE_RIGHT_BUTTON);
  return Pointer;
}

int InputGetScrollDelta(void) {
  return (int)GetMouseWheelMove();
}

Vec2 InputGetLookDelta(void) {
  return Vec2FromRL(GetMouseDelta());
}
