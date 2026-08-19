#ifndef RENDERER_H
#define RENDERER_H

#include "core/vecmath.h"
#include "player/player.h"
#include "world/world.h"
#include "render/backend.h"

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
void UnloadChunkMesh(Chunk *ChunkVal);
void DrawWorld(World *WorldVal, RenderCamera CameraVal);
void DrawAABBDebug(World *WorldVal, Player *PlayerVal);
void DrawPrefabSelection(Player *PlayerVal);
bool IsChunkInFrustum(RenderCamera CameraVal, Chunk *ChunkVal);

#endif
