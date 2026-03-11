#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>

typedef struct {
  bool wireframe;
  bool chunkBorders;
  bool AABB;
  bool freecam;
} DebugState;

extern DebugState g_debug;

#endif
