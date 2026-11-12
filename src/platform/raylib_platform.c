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
}

void PlatformShutdown(void) { CloseWindow(); }

bool PlatformShouldClose(void) { return WindowShouldClose(); }

float PlatformGetFrameTime(void) { return GetFrameTime(); }

double PlatformGetTime(void) { return GetTime(); }

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
