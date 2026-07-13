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
}

void PlatformShutdown(void) { CloseWindow(); }

bool PlatformShouldClose(void) { return WindowShouldClose(); }

float PlatformGetFrameTime(void) { return GetFrameTime(); }

double PlatformGetTime(void) { return GetTime(); }

void PlatformToggleFullscreen(void) { ToggleBorderlessWindowed(); }

void PlatformSetCursorDisabled(bool Disabled) {
  if (Disabled) {
    DisableCursor();
  } else {
    EnableCursor();
  }
}

int PlatformGetScreenWidth(void) { return GetScreenWidth(); }

int PlatformGetScreenHeight(void) { return GetScreenHeight(); }
