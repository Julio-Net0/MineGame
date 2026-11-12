#include "ui/debug.h"

DebugState *GetDebugState(void) {
  static DebugState SDebug = {
    .Wireframe = false,
    .ChunkBorders = false,
    .Aabb = false,
    .Freecam = false,
    .ForceCutoutShader = false,
    .SortChunks = true,
    .Occlusion = true,
  };
  return &SDebug;
}
