#ifndef PLATFORM_PLATFORM_H
#define PLATFORM_PLATFORM_H

#include <stdbool.h>

// Abstract platform/window layer. The engine drives the application shell —
// window lifecycle, the frame clock, the quit signal, fullscreen, cursor
// capture, and screen size — through these backend-agnostic calls. A single
// platform implementation TU (currently platform/raylib_platform.c) fulfils
// them. This header exposes no renderer (Raylib) types, so it can be
// implemented by any platform (e.g. a GLFW/Vulkan pairing later).

// Window lifecycle. PlatformInit creates the window (and, with the current
// Raylib backend, the graphics context the render backend draws into), so it
// must run before the render backend is initialized.
void PlatformInit(int Width, int Height, const char *Title);
void PlatformShutdown(void);

// Quit signal: true once the platform has requested the window to close.
bool PlatformShouldClose(void);

// Frame clock: last frame's delta time, and a monotonic wall-clock time since
// init. PlatformGetTime is always the wall clock, so deadlines, timeouts and
// work budgets keep measuring real time.
float PlatformGetFrameTime(void);
double PlatformGetTime(void);

// Client clock, read by fades, toasts and caret blinking. It follows the wall
// clock unless a fixed frame time is set: then every frame advances it, and
// PlatformGetFrameTime, by exactly that step, so what a frame shows depends on
// how many frames ran rather than on how fast the machine ran them. A scripted
// run sets it to make its screenshots reproducible.
void PlatformSetFixedFrameTime(float Seconds);
// Called once at the top of every frame; advances the fixed-step client clock.
void PlatformBeginFrame(void);
double PlatformGetClientTime(void);

// Presentation controls.
void PlatformToggleFullscreen(void);
void PlatformSetCursorDisabled(bool Disabled);

// Mouse cursor shapes for interactive UI elements.
typedef enum {
  PLATFORM_CURSOR_DEFAULT = 0,
  PLATFORM_CURSOR_IBEAM,
  PLATFORM_CURSOR_POINTING_HAND,
} PlatformCursorShape;

void PlatformSetCursorShape(PlatformCursorShape Shape);

// Screen dimensions in pixels.
int PlatformGetScreenWidth(void);
int PlatformGetScreenHeight(void);

// System clipboard access.
const char *PlatformGetClipboardText(void);
void PlatformSetClipboardText(const char *Text);

// Future seam: a Vulkan backend creates its device and swapchain from the
// native window handle, so a `void *PlatformGetNativeWindowHandle(void)` will
// be added here when a non-Raylib backend is written. Not implemented now — the
// Raylib backend creates and owns its context internally and does not need it.

#endif
