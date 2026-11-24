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

// --gl-max "3.3" or "4.3" caps the OpenGL version: the platform asks for no
// newer context, and the backend detects capabilities as if the driver stopped
// at that version even when it hands back a newer one (Windows drivers commonly
// return 4.6 for a 3.3 request). Encoded as major * 10 + minor.
//
// It is applied before --gl-tier: the version cap decides which tiers the
// "driver" supports, and --gl-tier can then only lower the tier further.
#define GL_VERSION_CAP_NONE 0
#define GL_VERSION_CAP_33 33
#define GL_VERSION_CAP_43 43

// --monitor is 1-based; 0 means the flag was not given (primary monitor). The
// platform clamps it to the monitors actually connected.
#define LAUNCH_MONITOR_DEFAULT 0
#define LAUNCH_MONITOR_MAX 16

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
  // GL_VERSION_CAP_NONE unless --gl-max was given.
  int GlVersionCap;
  // --hidden: the window is never shown nor focused; rendering and frame
  // capture work as usual. Implies windowed.
  bool Hidden;
  // LAUNCH_MONITOR_DEFAULT unless --monitor was given.
  int Monitor;
  // --lod off: the device is treated as unable to draw LOD, so no LOD store,
  // worker downsampling or GPU resource is ever created. Defaults to on.
  bool LodDisabled;
  // --lod-indirect-count off: the LOD cull writes a fixed command list with
  // culled draws zeroed instead of a compacted list drawn through
  // ARB_indirect_parameters, as on drivers without it. For testing that path.
  bool LodNoIndirectCount;
  // --lod-cull off: the LOD cull keeps everything (same command layout), so
  // culled and unculled frames can be compared pixel for pixel.
  bool LodNoCull;
  // --lod-hiz off: no two-phase Hi-Z occlusion in the LOD cull.
  bool LodNoHiz;
  // --lod-fog off: no fog at the LOD's edge. --lod-fog-end N: the horizontal
  // distance in blocks where the fog reaches the sky colour (it starts at
  // three quarters of it); 0 keeps LOD_FOG_END_DEFAULT.
  bool LodNoFog;
  int LodFogEnd;
  // --lod-reversed-z off: the LOD keeps a standard depth range even where
  // glClipControl allows reversed-Z.
  bool LodNoReversedZ;
  // --lod-arena-mb N: ceiling of the LOD vertex arena, which starts small
  // and grows on demand; 0 keeps LOD_ARENA_MB_DEFAULT.
  int LodArenaMb;
  // --lod-distance N: chunks out to which the LOD is generated and drawn; 0
  // keeps LOD_DISTANCE_DEFAULT. The fog end follows it unless --lod-fog-end
  // is given.
  int LodDistance;
  // --lod-gen off: no synthetic terrain; the LOD holds only what real chunks
  // brought (the golden comparisons run this way).
  bool LodNoGen;
  // --lod-cache off: no LOD disk cache. --lod-cache-dir P: the cache lives in
  // P instead of the save's lod/ directory, and is used even when the world
  // is read-only (scripts), which otherwise keeps it off. NULL when not given.
  bool LodNoCache;
  const char *LodCacheDir;
  // --lod-quality low|medium|high (world/lod/lod_quality.h); LOD_QUALITY_UNSET
  // keeps medium.
  int LodQuality;
  // --help / -h: print supported launch flags and exit.
  bool ShowHelp;
} LaunchOptions;

#define LOD_ARENA_MB_DEFAULT 192
#define LOD_ARENA_MB_MIN 4
#define LOD_ARENA_MB_MAX 2048
#define LOD_BYTES_PER_MB 1048576LL

#define LOD_DISTANCE_DEFAULT 512
#define LOD_DISTANCE_MIN 32
#define LOD_DISTANCE_MAX 2048
// The LOD distance the pause menu and /lod set at runtime: 0 (off) or
// LOD_RUNTIME_DISTANCE_MIN..LOD_DISTANCE_MAX.
#define LOD_RUNTIME_DISTANCE_MIN 128
#define LOD_QUALITY_UNSET (-1)
// The 1:1 render distance of an interactive run on a device that draws LOD
// when --render-distance is not given; scripts and devices without LOD keep
// DEFAULT_RENDER_DISTANCE.
#define LOD_INTERACTIVE_RENDER_DISTANCE 32
#define LOD_BLOCKS_PER_CHUNK 16

// The edge of the default LOD distance.
#define LOD_FOG_END_DEFAULT (LOD_DISTANCE_DEFAULT * LOD_BLOCKS_PER_CHUNK)
#define LOD_FOG_END_MIN 64
#define LOD_FOG_END_MAX 65536

void ParseLaunchOptions(int Argc, char **Argv);
void PrintLaunchHelp(const char *ProgramName);

// The LOD distance in chunks and the LOD fog end in blocks, defaults applied.
int LaunchLodDistance(void);
int LaunchLodFogEnd(void);
// Whether --lod-fog-end fixed the fog end; otherwise it follows the LOD
// distance, also when that changes at runtime.
bool LaunchLodFogEndLocked(void);
// The --lod-quality preset as a LodQuality value, medium when not given.
int LaunchLodQuality(void);
const LaunchOptions *GetLaunchOptions(void);
void SetAppliedCoreCount(int CoreCount);
void SetAppliedRamLimitMb(size_t RamLimitMb);

#endif
