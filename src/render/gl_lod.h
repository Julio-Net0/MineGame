#ifndef RENDER_GL_LOD_H
#define RENDER_GL_LOD_H

#include "render/backend.h"
#include "render/gl_api.h"
#include <stdbool.h>
#include <stdint.h>

// The GL 4.3 side of the LOD renderer, private to the backend. Exists only
// when the device can draw LOD; otherwise nothing here is created and the
// backend draws straight into the default framebuffer.
//
// The 1:1 passes keep drawing straight into the default framebuffer: on the
// development driver, rendering them into an offscreen target and copying it
// back is not bit-identical to drawing them in place, even with identical
// formats, so the 1:1 image never leaves the default framebuffer. Between the
// 1:1 opaque passes and the translucent pass, when there is LOD to draw:
//   1. the default depth (24/8) is copied into a texture of the same format;
//   2. a fullscreen triangle rewrites it into the LOD projection's depth;
//   3. the LOD is drawn into its own colour target against that depth;
//   4. a fullscreen triangle copies only the LOD's pixels onto the frame.
// A frame with no LOD to draw touches nothing, and one with LOD changes only
// the pixels the LOD covers.

// The frame's camera and the 1:1 projection's depth terms, from the backend.
typedef struct {
  RenderCamera Camera;
  int Width;
  int Height;
  // m10 and m14 of the 1:1 projection (see depth_remap.frag).
  float NearM10;
  float NearM14;
  GLuint BlockArrayTex;
  GLuint LodSampler;
  // Wireframe debug view: the LOD pass still fills (see GlLodDraw).
  bool Wireframe;
} GlLodFrame;

typedef struct {
  // The compacted command list drawn through ARB_indirect_parameters; without
  // it the cull writes a fixed list with culled commands zeroed.
  bool UseIndirectCount;
  // False keeps every command (--lod-cull off, a test aid); also turns HiZ off.
  bool Cull;
  // Two-phase Hi-Z occlusion (--lod-hiz).
  bool HiZ;
  // Fog toward FogColor (the sky) over the last quarter of FogEnd, the
  // horizontal distance in blocks (--lod-fog, --lod-fog-end).
  bool Fog;
  float FogEnd;
  float FogColor[3];
  // Reversed-Z (glClipControl) in the LOD passes.
  bool ReversedZ;
  // Ceiling of the vertex arena, which starts small and grows on demand.
  int64_t ArenaLimitBytes;
} GlLodOptions;

// Creates every LOD resource. False (and LOD stays off) when any of them
// cannot be created; nothing is left allocated then.
bool GlLodInit(GLuint SharedIndexBuffer, const GlLodOptions *Options);
void GlLodShutdown(void);
bool GlLodActive(void);

// Once per presented frame: recycles arena pages freed three frames ago.
void GlLodEndFrame(void);

RenderLodMesh GlLodUploadMesh(const ChunkVertex *Vertices,
                              const RenderLodRun *Runs, int RunCount,
                              int OpaqueRunCount);
void GlLodFreeMesh(RenderLodMesh Mesh);
uint32_t GlLodArenaGeneration(void);
void GlLodSetCoverage(int ColumnX, int ColumnZ,
                      const uint32_t Words[RENDER_LOD_COVERAGE_WORDS]);

// Depth copy and remap, the GPU cull (lod_cull.comp) and the opaque and border
// multi-draws (twice with HiZ: last frame's visible sections, then the Hi-Z
// pyramid and whatever it shows was missing), the blended pass of see-through
// faces, and the composite. Leaves the
// default framebuffer bound with the frame's usual state.
void GlLodDraw(const GlLodFrame *Frame, const RenderLodList *List);

// Reads the last cull's counters into the stats; stalls (see backend.h).
void GlLodReadCullStats(void);
// The fog of every LOD program at runtime (see GlLodOptions.Fog / FogEnd).
void GlLodSetFog(bool Enabled, float End);
void GlLodGetStats(RenderLodStats *Out);

#endif
