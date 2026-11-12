#ifndef RENDERER_H
#define RENDERER_H

#include "core/vecmath.h"
#include "player/player.h"
#include "world/world.h"
#include "render/backend.h"
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

bool RendererHasMeshJobCapacity(void);

// True while any mesh job slot is still occupied: queued, being built by a
// worker, or finished and waiting for this thread to upload it. A caller asking
// whether the world has finished loading needs this as well as the chunk flags,
// because a chunk stops being dirty the moment its job is queued, long before
// the mesh it produces reaches the GPU.
bool RendererHasPendingMeshWork(void);
void UnloadChunkMesh(Chunk *ChunkVal);
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
// accepted (Visible) and drew solid geometry (Rendered).
typedef struct {
  int Renderable;
  int Columns;
  int ColumnsOutside;
  int ColumnsInside;
  int ColumnsIntersecting;
  int ChunksTested;
  int Visible;
  int Rendered;
} RenderCullStats;

void GetLastCullStats(RenderCullStats *Out);

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
#endif

#endif
