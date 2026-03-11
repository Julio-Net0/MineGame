#include "debug.h"

DebugState g_debug = {
  .wireframe = false,
  .chunkBorders = false,
  .AABB = false,
  .freecam = false,
};

extern DebugState g_debug;
