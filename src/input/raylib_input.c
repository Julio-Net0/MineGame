#include "input/input.h"
#include "render/rl_compat.h"
#include "core/vecmath.h"
#include "raylib.h"

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

  #pragma unroll 4
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
  return System;
}

TextInput PollTextInput(void) {
  TextInput Text = {0};

  int Key = GetCharPressed();
  #pragma unroll 4
  for (int LoopIdx = 0; LoopIdx < CHAT_MAX_INPUT_CHARS; LoopIdx++) {
    if (Key <= 0) {
      break;
    }
    if ((Key >= ' ') && (Key <= '~') && (Text.Count < CHAT_MAX_INPUT_CHARS)) {
      Text.Chars[Text.Count] = (char)Key;
      Text.Count++;
    }
    Key = GetCharPressed();
  }

  Text.Backspace = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
  Text.Submit = IsKeyPressed(KEY_ENTER);
  Text.Cancel = IsKeyPressed(KEY_DELETE);
  return Text;
}

int InputGetScrollDelta(void) {
  return (int)GetMouseWheelMove();
}

Vec2 InputGetLookDelta(void) {
  return Vec2FromRL(GetMouseDelta());
}
