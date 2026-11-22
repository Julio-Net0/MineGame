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

// Creates every LOD resource. False (and LOD stays off) when any of them
// cannot be created; nothing is left allocated then.
bool GlLodInit(GLuint SharedIndexBuffer);
void GlLodShutdown(void);
bool GlLodActive(void);

// Once per presented frame: recycles arena pages freed three frames ago.
void GlLodEndFrame(void);

RenderLodMesh GlLodUploadMesh(const ChunkVertex *Vertices, int VertexCount);
void GlLodFreeMesh(RenderLodMesh Mesh);
void GlLodSetCoverage(int ColumnX, int ColumnZ,
                      const uint32_t Words[RENDER_LOD_COVERAGE_WORDS]);

// Depth copy and remap, the opaque and border multi-draws, and the composite.
// Leaves the default framebuffer bound with the frame's usual state.
void GlLodDraw(const GlLodFrame *Frame, const RenderLodDraw *Draws,
               int OpaqueCount, int BorderCount);

void GlLodGetStats(RenderLodStats *Out);

#endif
