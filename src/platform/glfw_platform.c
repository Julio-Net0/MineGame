#include "platform/platform.h"

#include "core/log.h"
#include "input/glfw_input_internal.h"
#include "platform/os_time.h"
#include "render/gl_api.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdlib.h>

// GLFW platform implementation: window, OpenGL context, frame clock, cursor
// and clipboard. Window placement, cursor capture and the borderless
// fullscreen toggle follow what the raylib platform did, so the game looks and
// behaves the same after the swap.

enum {
  GL_REQUIRED_MAJOR = 3,
  GL_REQUIRED_MINOR = 3
};

typedef struct {
  GLFWwindow *Window;
  GLFWcursor *IBeamCursor;
  GLFWcursor *HandCursor;
  PlatformCursorShape CursorShape;

  // Frame clock: the wall-clock time between consecutive presents.
  double LastPresentTime;
  float FrameTime;

  // A scripted run steps both clocks by a fixed amount per frame instead.
  float FixedFrameTime;
  double ClientTime;

  // Geometry to restore when leaving borderless fullscreen.
  bool Borderless;
  int WindowedX;
  int WindowedY;
  int WindowedWidth;
  int WindowedHeight;
} PlatformState;

static PlatformState *GetPlatformState(void) {
  static PlatformState State;
  return &State;
}

static void OnGlfwError(int Code, const char *Description) {
  LogWarn("GLFW: error %d: %s", Code, (Description != NULL) ? Description : "");
}

// Opening the game is not possible without the window or the context, and the
// rest of startup assumes both exist.
static void FailInit(const char *Reason) {
  LogError("PLATFORM: %s", Reason);
  glfwTerminate();
  exit(EXIT_FAILURE);
}

// Centred on the primary monitor, clamped to its top-left corner.
static void CentreWindow(GLFWwindow *Window, int Width, int Height) {
  GLFWmonitor *Monitor = glfwGetPrimaryMonitor();
  const GLFWvidmode *Mode = (Monitor != NULL) ? glfwGetVideoMode(Monitor) : NULL;
  if (Mode == NULL) {
    return;
  }
  int MonitorX = 0;
  int MonitorY = 0;
  glfwGetMonitorPos(Monitor, &MonitorX, &MonitorY);
  int PosX = MonitorX + ((Mode->width - Width) / 2);
  int PosY = MonitorY + ((Mode->height - Height) / 2);
  if (PosX < MonitorX) { PosX = MonitorX; }
  if (PosY < MonitorY) { PosY = MonitorY; }
  glfwSetWindowPos(Window, PosX, PosY);
}

void PlatformInit(int Width, int Height, const char *Title) {
  PlatformState *State = GetPlatformState();
  if (!OsAnchorWorkingDirectoryToExecutable()) {
    LogWarn("PLATFORM: could not change to the executable's directory");
  }

  glfwSetErrorCallback(OnGlfwError);
  if (glfwInit() != GLFW_TRUE) {
    FailInit("GLFW could not be initialised");
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, GL_REQUIRED_MAJOR);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, GL_REQUIRED_MINOR);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
  glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
  glfwWindowHint(GLFW_SAMPLES, 0);

  State->Window = glfwCreateWindow(Width, Height, (Title != NULL) ? Title : " ",
                                   NULL, NULL);
  if (State->Window == NULL) {
    FailInit("could not create a window with an OpenGL 3.3 core context; "
             "MineGame requires OpenGL 3.3");
  }
  CentreWindow(State->Window, Width, Height);

  glfwMakeContextCurrent(State->Window);
  int Version = gladLoadGL(glfwGetProcAddress);
  if (Version == 0 || GLAD_VERSION_MAJOR(Version) < GL_REQUIRED_MAJOR ||
      (GLAD_VERSION_MAJOR(Version) == GL_REQUIRED_MAJOR &&
       GLAD_VERSION_MINOR(Version) < GL_REQUIRED_MINOR)) {
    FailInit("the OpenGL context does not provide OpenGL 3.3; MineGame "
             "requires OpenGL 3.3");
  }
  glfwSwapInterval(0);

  State->IBeamCursor = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
  State->HandCursor = glfwCreateStandardCursor(GLFW_POINTING_HAND_CURSOR);
  State->CursorShape = PLATFORM_CURSOR_DEFAULT;

  GlfwInputAttach(State->Window);
  State->LastPresentTime = glfwGetTime();
  State->FrameTime = 0.0F;
}

void PlatformShutdown(void) {
  PlatformState *State = GetPlatformState();
  if (State->IBeamCursor != NULL) {
    glfwDestroyCursor(State->IBeamCursor);
    State->IBeamCursor = NULL;
  }
  if (State->HandCursor != NULL) {
    glfwDestroyCursor(State->HandCursor);
    State->HandCursor = NULL;
  }
  if (State->Window != NULL) {
    glfwDestroyWindow(State->Window);
    State->Window = NULL;
  }
  glfwTerminate();
}

bool PlatformShouldClose(void) {
  GLFWwindow *Window = GetPlatformState()->Window;
  return Window == NULL || glfwWindowShouldClose(Window) == GLFW_TRUE;
}

// Same order as before: swap, measure the frame, then pump events, so input
// read during the next frame is the input that arrived while this one ran.
void PlatformPresent(void) {
  PlatformState *State = GetPlatformState();
  glfwSwapBuffers(State->Window);

  double Now = glfwGetTime();
  State->FrameTime = (float)(Now - State->LastPresentTime);
  State->LastPresentTime = Now;

  GlfwInputBeginFrame();
  glfwPollEvents();
}

// Starts above zero so callers that use 0.0 as "never set" (the item-name
// toast) still tell an event on the first frame apart from no event.
#define FIXED_CLOCK_START_SECONDS 1.0

void PlatformSetFixedFrameTime(float Seconds) {
  PlatformState *State = GetPlatformState();
  State->FixedFrameTime = (Seconds > 0.0F) ? Seconds : 0.0F;
  State->ClientTime = FIXED_CLOCK_START_SECONDS;
}

void PlatformBeginFrame(void) {
  PlatformState *State = GetPlatformState();
  if (State->FixedFrameTime > 0.0F) {
    State->ClientTime += (double)State->FixedFrameTime;
  }
}

float PlatformGetFrameTime(void) {
  const PlatformState *State = GetPlatformState();
  return (State->FixedFrameTime > 0.0F) ? State->FixedFrameTime
                                        : State->FrameTime;
}

double PlatformGetTime(void) { return glfwGetTime(); }

double PlatformGetClientTime(void) {
  const PlatformState *State = GetPlatformState();
  return (State->FixedFrameTime > 0.0F) ? State->ClientTime : glfwGetTime();
}

// The monitor holding the window's centre, or the primary one.
static GLFWmonitor *CurrentMonitor(GLFWwindow *Window) {
  int WinX = 0;
  int WinY = 0;
  int WinW = 0;
  int WinH = 0;
  glfwGetWindowPos(Window, &WinX, &WinY);
  glfwGetWindowSize(Window, &WinW, &WinH);
  int CentreX = WinX + (WinW / 2);
  int CentreY = WinY + (WinH / 2);

  int Count = 0;
  GLFWmonitor **Monitors = glfwGetMonitors(&Count);
  for (int Idx = 0; Idx < Count; Idx++) {
    const GLFWvidmode *Mode = glfwGetVideoMode(Monitors[Idx]);
    if (Mode == NULL) {
      continue;
    }
    int MonX = 0;
    int MonY = 0;
    glfwGetMonitorPos(Monitors[Idx], &MonX, &MonY);
    if (CentreX >= MonX && CentreX < MonX + Mode->width && CentreY >= MonY &&
        CentreY < MonY + Mode->height) {
      return Monitors[Idx];
    }
  }
  return glfwGetPrimaryMonitor();
}

// Borderless fullscreen: an undecorated, always-on-top window covering the
// monitor it is on, rather than an exclusive video-mode change.
void PlatformToggleFullscreen(void) {
  PlatformState *State = GetPlatformState();
  GLFWwindow *Window = State->Window;
  if (Window == NULL) {
    return;
  }

  if (!State->Borderless) {
    GLFWmonitor *Monitor = CurrentMonitor(Window);
    const GLFWvidmode *Mode = (Monitor != NULL) ? glfwGetVideoMode(Monitor) : NULL;
    if (Mode == NULL) {
      return;
    }
    glfwGetWindowPos(Window, &State->WindowedX, &State->WindowedY);
    glfwGetWindowSize(Window, &State->WindowedWidth, &State->WindowedHeight);
    glfwSetWindowAttrib(Window, GLFW_DECORATED, GLFW_FALSE);
    glfwSetWindowAttrib(Window, GLFW_FLOATING, GLFW_TRUE);
    int MonitorX = 0;
    int MonitorY = 0;
    glfwGetMonitorPos(Monitor, &MonitorX, &MonitorY);
    glfwSetWindowPos(Window, MonitorX, MonitorY);
    glfwSetWindowSize(Window, Mode->width, Mode->height);
    glfwFocusWindow(Window);
    State->Borderless = true;
  } else {
    glfwSetWindowAttrib(Window, GLFW_FLOATING, GLFW_FALSE);
    glfwSetWindowAttrib(Window, GLFW_DECORATED, GLFW_TRUE);
    glfwSetWindowSize(Window, State->WindowedWidth, State->WindowedHeight);
    glfwSetWindowPos(Window, State->WindowedX, State->WindowedY);
    glfwFocusWindow(Window);
    State->Borderless = false;
  }
}

// The cursor is moved to the window centre on every capture change, and that
// jump is not reported as mouse movement.
static void CentreCursor(GLFWwindow *Window) {
  int Width = 0;
  int Height = 0;
  glfwGetWindowSize(Window, &Width, &Height);
  // Whole pixels, as the cursor was centred before.
  int CentreX = Width / 2;
  int CentreY = Height / 2;
  double X = (double)CentreX;
  double Y = (double)CentreY;
  glfwSetCursorPos(Window, X, Y);
  GlfwInputWarpCursor(X, Y);
}

void PlatformSetCursorDisabled(bool Disabled) {
  PlatformState *State = GetPlatformState();
  GLFWwindow *Window = State->Window;
  if (Window == NULL) {
    return;
  }
  bool RawSupported = glfwRawMouseMotionSupported() == GLFW_TRUE;
  if (Disabled) {
    glfwSetInputMode(Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    CentreCursor(Window);
    // Unaccelerated motion while the cursor drives the camera.
    if (RawSupported) {
      glfwSetInputMode(Window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
    PlatformSetCursorShape(PLATFORM_CURSOR_DEFAULT);
  } else {
    glfwSetInputMode(Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    CentreCursor(Window);
    if (RawSupported) {
      glfwSetInputMode(Window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
  }
}

void PlatformSetCursorShape(PlatformCursorShape Shape) {
  PlatformState *State = GetPlatformState();
  if (Shape == State->CursorShape || State->Window == NULL) {
    return;
  }
  State->CursorShape = Shape;
  GLFWcursor *Cursor = NULL;
  switch (Shape) {
  case PLATFORM_CURSOR_IBEAM:
    Cursor = State->IBeamCursor;
    break;
  case PLATFORM_CURSOR_POINTING_HAND:
    Cursor = State->HandCursor;
    break;
  case PLATFORM_CURSOR_DEFAULT:
  default:
    Cursor = NULL;
    break;
  }
  glfwSetCursor(State->Window, Cursor);
}

int PlatformGetScreenWidth(void) {
  int Width = 0;
  GLFWwindow *Window = GetPlatformState()->Window;
  if (Window != NULL) {
    glfwGetWindowSize(Window, &Width, NULL);
  }
  return Width;
}

int PlatformGetScreenHeight(void) {
  int Height = 0;
  GLFWwindow *Window = GetPlatformState()->Window;
  if (Window != NULL) {
    glfwGetWindowSize(Window, NULL, &Height);
  }
  return Height;
}

void PlatformGetFramebufferSize(int *OutWidth, int *OutHeight) {
  int Width = 0;
  int Height = 0;
  GLFWwindow *Window = GetPlatformState()->Window;
  if (Window != NULL) {
    glfwGetFramebufferSize(Window, &Width, &Height);
  }
  if (OutWidth != NULL) { *OutWidth = Width; }
  if (OutHeight != NULL) { *OutHeight = Height; }
}

const char *PlatformGetClipboardText(void) {
  GLFWwindow *Window = GetPlatformState()->Window;
  const char *Text = (Window != NULL) ? glfwGetClipboardString(Window) : NULL;
  return (Text != NULL) ? Text : "";
}

void PlatformSetClipboardText(const char *Text) {
  GLFWwindow *Window = GetPlatformState()->Window;
  if (Window != NULL && Text != NULL) {
    glfwSetClipboardString(Window, Text);
  }
}

void *PlatformGetNativeWindowHandle(void) {
  return (void *)GetPlatformState()->Window;
}
