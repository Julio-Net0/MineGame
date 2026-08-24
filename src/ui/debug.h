#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>

typedef struct {
  bool Wireframe;
  bool ChunkBorders;
  bool Aabb;
  bool Freecam;

  // Measurement aid, not a rendering feature. Forces the solid-block pass onto
  // the cutout fragment shader, which is the pre-split behaviour: a shader
  // containing `discard` costs the hardware early depth rejection for the whole
  // draw. Toggling it turns the shader split on and off at runtime so the GPU
  // time it buys can be measured on one machine in one sitting, which is the
  // only way to see it -- the development machine's GPU is not the bottleneck.
  bool ForceCutoutShader;
} DebugState;

DebugState *GetDebugState(void);

#endif
