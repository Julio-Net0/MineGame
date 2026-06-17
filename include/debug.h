#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>

typedef struct {
  bool Wireframe;
  bool ChunkBorders;
  bool Aabb;
  bool Freecam;
} DebugState;

DebugState *GetDebugState(void);

#endif
