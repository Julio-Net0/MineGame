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
  return System;
}

Vec2 InputGetLookDelta(void) {
  return Vec2FromRL(GetMouseDelta());
}
