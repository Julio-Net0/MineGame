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

  // Measurement aid, like ForceCutoutShader above, and the only flag here that
  // defaults to enabled -- it names an optimization rather than an overlay, so
  // clearing it is what costs something. Off, the solid and cutout passes walk
  // chunk slots in whatever order the hash map left them, which is what they did
  // before the ordering existed. The translucent pass ignores it: unordered
  // blending is a rendering fault, not an alternative worth measuring.
  bool SortChunks;

  // Measurement aid, enabled by default like SortChunks. Off, the world draw
  // skips the occlusion traversal and draws every frustum-visible chunk, so the
  // two can be compared in one sitting.
  bool Occlusion;

  // Measurement aid, enabled by default. Applies only while Occlusion is on and
  // the traversal fell back because sight lines reach the open sky: the world
  // draw then sweeps columns near to far and skips those wholly below a
  // conservative horizon built from fully opaque chunk layers.
  bool HorizonCulling;

  // Set for scripted runs only. Draw order otherwise follows upload order in two
  // places -- chunks at the same distance from the camera, and the opaque passes'
  // grouping by GPU memory block -- and upload order depends on worker timing.
  // Wireframe lines and the odd seam pixel then change between runs. Ordering
  // ties by chunk coordinates and submitting every pass in that exact order makes
  // screenshots repeatable at a cost normal play skips.
  bool StableDrawOrder;
} DebugState;

DebugState *GetDebugState(void);

#endif
