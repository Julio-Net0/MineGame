#include "input/input.h"
#include "input/glfw_input_internal.h"
#include "core/utils.h"
#include "core/vecmath.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <math.h>
#include <string.h>

// GLFW input implementation. Callbacks record events as they arrive during the
// platform's event pump; GlfwInputBeginFrame, called just before each pump,
// rolls the frame over. Every query below therefore reads a snapshot that stays
// fixed for the whole frame, so a key reported pressed once is pressed for
// every caller that asks during that frame and for no frame after it.

enum {
  INPUT_KEY_COUNT = GLFW_KEY_LAST + 1,
  INPUT_BUTTON_COUNT = GLFW_MOUSE_BUTTON_LAST + 1,
  INPUT_CHAR_QUEUE = CHAT_MAX_INPUT_CHARS,
  DELETE_CODEPOINT = 127
};

typedef struct {
  bool KeyDown[INPUT_KEY_COUNT];
  // Edges are recorded from the events themselves rather than derived from
  // state, so a key pressed and released within one frame still registers.
  bool KeyPressed[INPUT_KEY_COUNT];
  bool KeyRepeated[INPUT_KEY_COUNT];

  bool ButtonDown[INPUT_BUTTON_COUNT];
  bool ButtonPressed[INPUT_BUTTON_COUNT];
  bool ButtonReleased[INPUT_BUTTON_COUNT];

  unsigned int Chars[INPUT_CHAR_QUEUE];
  int CharCount;

  float WheelX;
  float WheelY;

  // Single precision, as positions were stored before, so the look delta of
  // a given mouse movement is the same number it always was.
  float CursorX;
  float CursorY;
  float PreviousCursorX;
  float PreviousCursorY;
} InputState;

static InputState *GetInputState(void) {
  static InputState State;
  return &State;
}

static void OnKey(GLFWwindow *Window, int Key, int Scancode, int Action,
                  int Mods) {
  (void)Window;
  (void)Scancode;
  (void)Mods;
  if (Key < 0 || Key >= INPUT_KEY_COUNT) {
    return;
  }
  InputState *State = GetInputState();
  if (Action == GLFW_PRESS) {
    State->KeyDown[Key] = true;
    State->KeyPressed[Key] = true;
  } else if (Action == GLFW_RELEASE) {
    State->KeyDown[Key] = false;
  } else if (Action == GLFW_REPEAT) {
    State->KeyRepeated[Key] = true;
  }
}

static void OnChar(GLFWwindow *Window, unsigned int Codepoint) {
  (void)Window;
  InputState *State = GetInputState();
  if (State->CharCount < INPUT_CHAR_QUEUE) {
    State->Chars[State->CharCount++] = Codepoint;
  }
}

static void OnMouseButton(GLFWwindow *Window, int Button, int Action, int Mods) {
  (void)Window;
  (void)Mods;
  if (Button < 0 || Button >= INPUT_BUTTON_COUNT) {
    return;
  }
  InputState *State = GetInputState();
  if (Action == GLFW_PRESS) {
    State->ButtonDown[Button] = true;
    State->ButtonPressed[Button] = true;
  } else if (Action == GLFW_RELEASE) {
    State->ButtonDown[Button] = false;
    State->ButtonReleased[Button] = true;
  }
}

static void OnCursorPos(GLFWwindow *Window, double X, double Y) {
  (void)Window;
  InputState *State = GetInputState();
  State->CursorX = (float)X;
  State->CursorY = (float)Y;
}

static void OnScroll(GLFWwindow *Window, double XOffset, double YOffset) {
  (void)Window;
  InputState *State = GetInputState();
  State->WheelX += (float)XOffset;
  State->WheelY += (float)YOffset;
}

void GlfwInputAttach(GLFWwindow *Window) {
  memset(GetInputState(), 0, sizeof(InputState));
  glfwSetKeyCallback(Window, OnKey);
  glfwSetCharCallback(Window, OnChar);
  glfwSetMouseButtonCallback(Window, OnMouseButton);
  glfwSetCursorPosCallback(Window, OnCursorPos);
  glfwSetScrollCallback(Window, OnScroll);
}

void GlfwInputBeginFrame(void) {
  InputState *State = GetInputState();
  memset(State->KeyPressed, 0, sizeof(State->KeyPressed));
  memset(State->KeyRepeated, 0, sizeof(State->KeyRepeated));
  memset(State->ButtonPressed, 0, sizeof(State->ButtonPressed));
  memset(State->ButtonReleased, 0, sizeof(State->ButtonReleased));
  State->CharCount = 0;
  State->WheelX = 0.0F;
  State->WheelY = 0.0F;
  State->PreviousCursorX = State->CursorX;
  State->PreviousCursorY = State->CursorY;
}

void GlfwInputWarpCursor(double X, double Y) {
  InputState *State = GetInputState();
  State->CursorX = (float)X;
  State->CursorY = (float)Y;
  State->PreviousCursorX = State->CursorX;
  State->PreviousCursorY = State->CursorY;
}

static bool IsKeyDown(int Key) { return GetInputState()->KeyDown[Key]; }

static bool IsKeyPressed(int Key) { return GetInputState()->KeyPressed[Key]; }

// Pressed this frame, or held long enough for the OS to repeat it.
static bool IsKeyPressedOrRepeated(int Key) {
  const InputState *State = GetInputState();
  return State->KeyPressed[Key] || State->KeyRepeated[Key];
}

static bool IsControlDown(void) {
  return IsKeyDown(GLFW_KEY_LEFT_CONTROL) || IsKeyDown(GLFW_KEY_RIGHT_CONTROL);
}

static bool IsShiftDown(void) {
  return IsKeyDown(GLFW_KEY_LEFT_SHIFT) || IsKeyDown(GLFW_KEY_RIGHT_SHIFT);
}

// The wheel reports on whichever axis moved more, so a horizontal tilt still
// scrolls.
static float WheelMove(void) {
  const InputState *State = GetInputState();
  return (fabsf(State->WheelX) > fabsf(State->WheelY)) ? State->WheelX
                                                       : State->WheelY;
}

// -1, or the 0-based hotbar slot whose number key was pressed this frame.
static int PressedHotbarKey(void) {
  int Slot = -1;
  for (int IdxI = 0; IdxI < HOTBAR_SIZE; IdxI++) {
    if (IsKeyPressed(GLFW_KEY_1 + IdxI)) {
      Slot = IdxI;
    }
  }
  return Slot;
}

PlayerInput PollPlayerInput(bool HasControl) {
  PlayerInput Input = {0};
  Input.HotbarSelect = -1;

  if (!HasControl) {
    return Input;
  }

  if (IsKeyDown(GLFW_KEY_W)) { Input.MoveForward += 1.0F; }
  if (IsKeyDown(GLFW_KEY_S)) { Input.MoveForward -= 1.0F; }
  if (IsKeyDown(GLFW_KEY_A)) { Input.MoveX -= 1.0F; }
  if (IsKeyDown(GLFW_KEY_D)) { Input.MoveX += 1.0F; }

  Input.Jump = IsKeyPressed(GLFW_KEY_SPACE);
  Input.AscendHeld = IsKeyDown(GLFW_KEY_SPACE);
  Input.DescendHeld = IsKeyDown(GLFW_KEY_LEFT_SHIFT);
  Input.SprintHeld = IsControlDown();

  Input.HotbarSelect = PressedHotbarKey();
  Input.HotbarScroll = (int)WheelMove();

  const InputState *State = GetInputState();
  Input.Break = State->ButtonPressed[GLFW_MOUSE_BUTTON_LEFT];
  Input.Place = State->ButtonPressed[GLFW_MOUSE_BUTTON_RIGHT];
  Input.PickBlock = State->ButtonPressed[GLFW_MOUSE_BUTTON_MIDDLE];

  return Input;
}

SystemInput PollSystemInput(void) {
  SystemInput System = {0};
  System.FullscreenToggle = IsKeyPressed(GLFW_KEY_F11);
  System.DebugToggle = IsKeyPressed(GLFW_KEY_F3);
  System.ChatOpen = IsKeyPressed(GLFW_KEY_T);
  System.CommandOpen = IsKeyPressed(GLFW_KEY_SLASH);
  System.PauseToggle = IsKeyPressed(GLFW_KEY_ESCAPE);
  System.InventoryToggle = IsKeyPressed(GLFW_KEY_E);
  System.HotbarKey = PressedHotbarKey();
  return System;
}

TextInput PollTextInput(void) {
  TextInput Text = {0};
  const InputState *State = GetInputState();

  bool Ctrl = IsControlDown();

  // Any printable codepoint, encoded as UTF-8, so accented letters typed on
  // the keyboard reach the chat. Only whole sequences are appended.
  for (int Idx = 0; Idx < State->CharCount; Idx++) {
    unsigned int Codepoint = State->Chars[Idx];
    if (Codepoint < ' ' || Codepoint == DELETE_CODEPOINT || Ctrl) {
      continue;
    }
    char Encoded[UTF8_MAX_BYTES];
    int Size = EncodeUtf8(Codepoint, Encoded);
    if (Size > 0 && Text.Count + Size <= CHAT_MAX_INPUT_CHARS) {
      memcpy(&Text.Chars[Text.Count], Encoded, (size_t)Size);
      Text.Count += Size;
    }
  }

  Text.Backspace = IsKeyPressedOrRepeated(GLFW_KEY_BACKSPACE);
  Text.Delete = IsKeyPressedOrRepeated(GLFW_KEY_DELETE);
  Text.Submit = IsKeyPressed(GLFW_KEY_ENTER);
  Text.Cancel = IsKeyPressed(GLFW_KEY_ESCAPE);
  Text.MoveLeft = IsKeyPressedOrRepeated(GLFW_KEY_LEFT);
  Text.MoveRight = IsKeyPressedOrRepeated(GLFW_KEY_RIGHT);
  Text.MoveHome = IsKeyPressed(GLFW_KEY_HOME);
  Text.MoveEnd = IsKeyPressed(GLFW_KEY_END);
  Text.WordStep = Ctrl;
  Text.ShiftHeld = IsShiftDown();

  if (Ctrl) {
    Text.SelectAll = IsKeyPressed(GLFW_KEY_A);
    Text.Copy = IsKeyPressed(GLFW_KEY_C);
    Text.Paste = IsKeyPressed(GLFW_KEY_V);
    Text.Cut = IsKeyPressed(GLFW_KEY_X);
    Text.Undo = IsKeyPressed(GLFW_KEY_Z);
  }

  Text.HistoryPrev = IsKeyPressedOrRepeated(GLFW_KEY_UP);
  Text.HistoryNext = IsKeyPressedOrRepeated(GLFW_KEY_DOWN);
  Text.Autocomplete = IsKeyPressed(GLFW_KEY_TAB);

  return Text;
}

PointerInput PollPointerInput(void) {
  const InputState *State = GetInputState();
  PointerInput Pointer = {0};
  Pointer.Cursor = (Vec2){State->CursorX, State->CursorY};
  Pointer.LeftPressed = State->ButtonPressed[GLFW_MOUSE_BUTTON_LEFT];
  Pointer.LeftDown = State->ButtonDown[GLFW_MOUSE_BUTTON_LEFT];
  Pointer.LeftReleased = State->ButtonReleased[GLFW_MOUSE_BUTTON_LEFT];
  Pointer.RightPressed = State->ButtonPressed[GLFW_MOUSE_BUTTON_RIGHT];
  Pointer.RightDown = State->ButtonDown[GLFW_MOUSE_BUTTON_RIGHT];
  Pointer.RightReleased = State->ButtonReleased[GLFW_MOUSE_BUTTON_RIGHT];
  return Pointer;
}

int InputGetScrollDelta(void) { return (int)WheelMove(); }

Vec2 InputGetLookDelta(void) {
  const InputState *State = GetInputState();
  return (Vec2){State->CursorX - State->PreviousCursorX,
                State->CursorY - State->PreviousCursorY};
}

bool InputIsShiftDown(void) { return IsShiftDown(); }
