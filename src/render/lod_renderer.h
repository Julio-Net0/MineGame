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
// chunks) and every child under its data is resident, or missing from a column
// generated whole and so holding nothing; one wholly covered by 1:1
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

// The refine factor (world/lod/lod_quality.h) at runtime: the next draw redoes
// the selection with it; cached meshes are kept.
void LodRendererSetRefineFactor(int Factor);
int LodRendererRefineFactor(void);

// The 1:1 coverage of chunk columns. A column cuts the LOD away only once it is
// whole: every chunk the world created in it has resolved and some hold
// terrain. Expect: the world created the chunk. SetChunkCovered: it resolved,
// meshed or without a mesh, with terrain (Covered) or without. Forget: it left
// the world.
void LodRendererExpectChunk(int ChunkX, int ChunkY, int ChunkZ);
void LodRendererSetChunkCovered(int ChunkX, int ChunkY, int ChunkZ, bool Covered);
void LodRendererForgetChunk(int ChunkX, int ChunkY, int ChunkZ);

// Between the 1:1 cutout pass and the translucent pass.
void LodRendererDraw(RenderCamera Camera);

// No selected section waits for a mesh, and no mesh job is still running
// (always true when off or not drawing).
bool LodRendererIdle(void);

// Worker side of a section mesh job: meshes the input the main thread
// prepared, with the worker's mesher scratch (see SetChunkWorkerLodMeshRunner).
void LodRendererRunMeshJob(int JobId, int MesherSlot);

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
