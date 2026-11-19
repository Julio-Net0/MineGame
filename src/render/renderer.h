#ifndef RENDERER_H
#define RENDERER_H

#include "core/vecmath.h"
#include "player/player.h"
#include "world/world.h"
#include "render/backend.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  FACE_TOP,
  FACE_BOTTOM,
  FACE_LEFT,
  FACE_RIGHT,
  FACE_FRONT,
  FACE_BACK
} BlockFace;

void InitRenderer(void);
void CloseRenderer(void);

// Allocates the mesher scratch of Count mesh workers, once, before they start.
// Returns how many slots were allocated, at most Count; fewer only when memory
// ran out, in which case only that many workers may be started.
int RendererInitMesherSlots(int Count);
void RendererFreeMesherSlots(void);
// Main thread: capture the chunk's snapshot into a free job slot and return its
// id, or -1 when the chunk needs no mesh or no slot is free.
int RendererQueueMeshJob(World *WorldVal, Chunk *ChunkVal);

// Worker thread: run the passes for a queued job using the mesher scratch of
// MesherSlot. Issues no graphics call.
void RendererBuildMeshJob(int JobId, int MesherSlot);

// Main thread: upload one finished mesh and release its slot. False when none
// is ready.
bool RendererUploadFinishedMesh(void);

// Release a claimed slot that never reached the work queue.
void RendererAbandonMeshJob(int JobId);

// Main thread: resolves a chunk with no solid blocks and no unloaded rock beside
// it to no mesh with every face visible, clearing its dirty flag, and returns
// true. That result reads no loaded neighbour, so it needs no
// neighbour-readiness check first. Returns false and does nothing for any
// other chunk.
bool RendererResolveEmptyChunk(World *WorldVal, Chunk *ChunkVal);

// Number of mesh job slots free to be claimed. Only the main thread claims
// slots, so for that thread the count can only grow until it claims one.
int RendererCountFreeMeshJobs(void);

// True while any mesh job slot is still occupied: queued, being built by a
// worker, or finished and waiting for this thread to upload it. A caller asking
// whether the world has finished loading needs this as well as the chunk flags,
// because a chunk stops being dirty the moment its job is queued, long before
// the mesh it produces reaches the GPU.
bool RendererHasPendingMeshWork(void);
void UnloadChunkMesh(Chunk *ChunkVal);

// Binds the renderer's per-slot chunk bookkeeping to the world's slot array.
// Call once after InitWorld.
void RendererAttachWorld(const World *WorldVal);

// Eviction hook: releases the chunk's meshes and drops every per-slot record the
// renderer keeps for it, so nothing follows the slot to its next occupant.
void RendererForgetChunk(Chunk *ChunkVal);

void DrawWorld(World *WorldVal, RenderCamera CameraVal);
void DrawAABBDebug(World *WorldVal, Player *PlayerVal);
void DrawPrefabSelection(Player *PlayerVal);
// How many chunk meshes the last DrawWorld actually submitted. Reported by the
// debug overlay, which used to re-run the visibility test over every chunk to
// find out -- a second full scan to recompute something the draw loop had just
// finished counting.
int GetLastRenderedChunkCount(void);

// What the last DrawWorld's visibility pass did: how many chunks own a mesh and
// how many columns they form, how the column boxes classified against the
// frustum, how many chunks still needed an individual test, and how many were
// accepted by the frustum (Visible) and drew solid geometry (Rendered).
//
// Occluded is how many of the Visible chunks the occlusion traversal rejected;
// OcclusionNodes and OcclusionReachable are the traversal's work and reach, and
// OcclusionFallback is set when the frame fell back to frustum culling alone:
// the camera stood outside the loaded set or inside rock, or a sight line
// reached the open sky above the loaded columns. HorizonActive is set when such a
// sky fallback ran the horizon sweep instead, and HorizonCulled is how many of
// the Visible chunks it rejected. SubmittedVertices sums every mesh
// the passes actually drew -- a machine-independent proxy for GPU load.
typedef struct {
  int Renderable;
  int Columns;
  int ColumnsOutside;
  int ColumnsInside;
  int ColumnsIntersecting;
  int ChunksTested;
  int Visible;
  int Rendered;
  int Occluded;
  int OcclusionNodes;
  int OcclusionReachable;
  int HorizonCulled;
  int64_t SubmittedVertices;
  bool OcclusionEnabled;
  bool OcclusionFallback;
  bool HorizonEnabled;
  bool HorizonActive;
} RenderCullStats;

void GetLastCullStats(RenderCullStats *Out);

// The horizon cull's occluders: per renderable column, the topmost stacks of
// fully opaque chunk layers. Columns counts the renderable columns, Runs and
// RunLayers every cached run and its height in blocks, TopRunLayers only each
// column's topmost run. Refreshed is how many stale columns this query had to
// recompute; the query refreshes them, which the draw would otherwise do.
typedef struct {
  int Columns;
  int ColumnsWithRun;
  int Runs;
  int Refreshed;
  int64_t RunLayers;
  int64_t TopRunLayers;
} OccluderRunStats;

void RendererGetOccluderRunStats(World *WorldVal, OccluderRunStats *Out);

// Vertices currently uploaded for chunk meshes, per mesh target. Machine
// independent, unlike timings, so scripts compare geometry changes with it.
typedef struct {
  int64_t Opaque;
  int64_t Cutout;
  int64_t Translucent;
} MeshVertexTotals;

void RendererGetMeshVertexTotals(MeshVertexTotals *Out);

#ifdef MINEGAME_PROFILE
// Recounts the loaded chunks owning a mesh and checks that each is in the
// renderer's renderable set exactly once. Logs the outcome.
bool RendererCheckRenderableSet(World *WorldVal);

// Logs how many loaded chunks are fully open, partially open, sealed or not
// yet analysed by the face connectivity pass.
void RendererLogVisibilityStats(const World *WorldVal);
#endif

#endif
