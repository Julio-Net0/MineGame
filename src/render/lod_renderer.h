#ifndef RENDER_LOD_RENDERER_H
#define RENDER_LOD_RENDERER_H

#include "render/backend.h"
#include <stdbool.h>
#include <stdint.h>

// Draws the client's LOD hierarchy beyond the 1:1 chunks. API-agnostic: it
// picks sections, meshes them with the voxel mesher and hands draws to the
// backend; the GL side lives in gl_lod.c.
//
// Selection walks down from the coarsest level and draws every region at
// exactly one level, so no two levels ever overlap. A section is refined into
// its eight children while the camera is close (or while it overlaps 1:1
// chunks) and every child holding data is resident; one wholly covered by 1:1
// chunks is not drawn at all; one partly covered is drawn with the border
// shader, which drops the fragments inside 1:1 chunks.

// After the backend and LOD ingestion exist; does nothing unless both are on.
void LodRendererInit(void);
void LodRendererShutdown(void);
bool LodRendererEnabled(void);

// Turns drawing on or off at runtime (/lod, script "lod"). The store keeps
// filling and the 1:1 coverage stays current while off, so drawing resumes
// where it stopped. On by default.
void LodRendererSetDrawOn(bool On);
bool LodRendererDrawOn(void);

// A chunk now drawn 1:1 (meshed, or resolved without a mesh) or no longer.
void LodRendererSetChunkCovered(int ChunkX, int ChunkY, int ChunkZ, bool Covered);

// Between the 1:1 cutout pass and the translucent pass.
void LodRendererDraw(RenderCamera Camera);

// No selected section waits for a mesh (always true when off).
bool LodRendererIdle(void);

typedef struct {
  bool Enabled;
  bool DrawOn;
  int Selected;
  int OpaqueSections;
  int BorderSections;
  int MeshedSections;
  int WaitingSections;
  // Selected sections left without a mesh because the vertex arena was full.
  int StarvedSections;
  // Selected meshes freed so far, the farthest first, to make room for nearer
  // starved sections.
  int64_t EvictedForRoom;
  int CachedMeshes;
  // The backend's view of the last pass: arena use, candidate vertices and the
  // GPU cull's (late) results.
  RenderLodStats Gpu;
} LodRenderStats;

void LodRendererGetStats(LodRenderStats *Out);

// Refreshes the GPU cull's counters in the stats, which stalls the frame (see
// RenderLodReadCullStats). Unless Force, at most every half second, for the
// debug overlay.
void LodRendererReadCullStats(bool Force);

#endif
