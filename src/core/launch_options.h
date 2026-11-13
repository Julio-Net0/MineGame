#ifndef CORE_LAUNCH_OPTIONS_H
#define CORE_LAUNCH_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INITIAL_WIDTH 1280
#define INITIAL_HEIGHT 720

// How the window is sized at startup.
//
// Fullscreen is the normal path and the framebuffer is then the monitor's, which
// is what makes the default INITIAL_WIDTH/HEIGHT above nearly invisible. An
// explicit size exists to measure whether GPU time scales with pixel count:
// fill-rate cost does, cost bound on vertex bandwidth does not, and running the
// same camera position at two very different framebuffer sizes separates them.
typedef struct {
  int Width;
  int Height;
  bool Fullscreen;
} WindowRequest;

// --gl-tier caps the render tier the backend may select (0, 1 or 2); without
// the flag the backend picks the highest tier the driver supports.
#define GL_TIER_CAP_NONE (-1)
#define GL_TIER_CAP_MAX 2

#define SCRIPT_DEFAULT_OUT_DIR "automation_out"
#define SCRIPT_DEFAULT_TIMEOUT_SECONDS 120.0

// Process-lifetime window, low-spec simulation and automation options parsed
// from the command line. Zero (or 1.0 for CpuSlowdown) means the flag was not
// given and that subsystem runs at its default. ScriptPath is null unless
// --script was given, which is what puts the run in script mode; the two other
// script fields always hold a usable value, so the runner never has to decide
// between a flag's value and its default.
//
// The string fields point into argv, which lives for the whole process.
typedef struct {
  WindowRequest Window;
  int WorkerCount;
  int CoreCount;
  double CpuSlowdown;
  size_t RamLimitMb;
  // Initial render distance in chunks; 0 when --render-distance was not given.
  int RenderDistance;
  bool HasSeed;
  uint64_t Seed;
  const char *ScriptPath;
  const char *ScriptOutDir;
  double ScriptTimeoutSeconds;
  // GL_TIER_CAP_NONE unless --gl-tier was given.
  int GlTierCap;
} LaunchOptions;

void ParseLaunchOptions(int Argc, char **Argv);
const LaunchOptions *GetLaunchOptions(void);
void SetAppliedCoreCount(int CoreCount);
void SetAppliedRamLimitMb(size_t RamLimitMb);

#endif
