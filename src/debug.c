#include "debug.h"

DebugState *GetDebugState(void) {
  static DebugState SDebug = {
    .Wireframe = false,
    .ChunkBorders = false,
    .Aabb = false,
    .Freecam = false,
  };
  return &SDebug;
}
