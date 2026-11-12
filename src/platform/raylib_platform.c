#include "platform/platform.h"

#include "raylib.h"

// Raylib platform implementation. Confines all application-shell window, clock,
// cursor, and screen Raylib calls to this single TU. PlatformInit also performs
// the platform-internal setup previously inlined in main.c: log verbosity and
// anchoring the working directory to the executable so assets resolve.

void PlatformInit(int Width, int Height, const char *Title) {
  SetTraceLogLevel(LOG_WARNING);
  ChangeDirectory(GetApplicationDirectory());
  InitWindow(Width, Height, Title);
  // ESC opens the pause menu; the window close button is the quit signal.
  SetExitKey(KEY_NULL);
  // Raylib measures each frame from BeginDrawing to EndDrawing, so the first
  // frame is opened here and every present closes one and opens the next.
  BeginDrawing();
}

void PlatformPresent(void) {
  EndDrawing();
  BeginDrawing();
}

void PlatformGetFramebufferSize(int *OutWidth, int *OutHeight) {
  if (OutWidth != (int *)0) { *OutWidth = GetRenderWidth(); }
  if (OutHeight != (int *)0) { *OutHeight = GetRenderHeight(); }
}

void PlatformShutdown(void) { CloseWindow(); }

bool PlatformShouldClose(void) { return WindowShouldClose(); }

typedef struct {
  float FixedFrameTime;
  double ClientTime;
} FixedClockState;

static FixedClockState *GetFixedClockState(void) {
  static FixedClockState State;
  return &State;
}

// Starts above zero so callers that use 0.0 as "never set" (the item-name
// toast) still tell an event on the first frame apart from no event.
#define FIXED_CLOCK_START_SECONDS 1.0

void PlatformSetFixedFrameTime(float Seconds) {
  FixedClockState *State = GetFixedClockState();
  State->FixedFrameTime = (Seconds > 0.0F) ? Seconds : 0.0F;
  State->ClientTime = FIXED_CLOCK_START_SECONDS;
}

void PlatformBeginFrame(void) {
  FixedClockState *State = GetFixedClockState();
  if (State->FixedFrameTime > 0.0F) {
    State->ClientTime += (double)State->FixedFrameTime;
  }
}

float PlatformGetFrameTime(void) {
  const FixedClockState *State = GetFixedClockState();
  return (State->FixedFrameTime > 0.0F) ? State->FixedFrameTime : GetFrameTime();
}

double PlatformGetTime(void) { return GetTime(); }

double PlatformGetClientTime(void) {
  const FixedClockState *State = GetFixedClockState();
  return (State->FixedFrameTime > 0.0F) ? State->ClientTime : GetTime();
}

void PlatformToggleFullscreen(void) { ToggleBorderlessWindowed(); }

static PlatformCursorShape CurrentCursorShape = PLATFORM_CURSOR_DEFAULT;

void PlatformSetCursorDisabled(bool Disabled) {
  if (Disabled) {
    DisableCursor();
    CurrentCursorShape = PLATFORM_CURSOR_DEFAULT;
    SetMouseCursor(MOUSE_CURSOR_DEFAULT);
  } else {
    EnableCursor();
  }
}

void PlatformSetCursorShape(PlatformCursorShape Shape) {
  if (Shape == CurrentCursorShape) {
    return;
  }
  CurrentCursorShape = Shape;
  int RaylibCursor = MOUSE_CURSOR_DEFAULT;
  switch (Shape) {
  case PLATFORM_CURSOR_IBEAM:
    RaylibCursor = MOUSE_CURSOR_IBEAM;
    break;
  case PLATFORM_CURSOR_POINTING_HAND:
    RaylibCursor = MOUSE_CURSOR_POINTING_HAND;
    break;
  case PLATFORM_CURSOR_DEFAULT:
  default:
    RaylibCursor = MOUSE_CURSOR_DEFAULT;
    break;
  }
  SetMouseCursor(RaylibCursor);
}

int PlatformGetScreenWidth(void) { return GetScreenWidth(); }

int PlatformGetScreenHeight(void) { return GetScreenHeight(); }

const char *PlatformGetClipboardText(void) { return GetClipboardText(); }

void PlatformSetClipboardText(const char *Text) { SetClipboardText(Text); }
