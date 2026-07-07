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
void BuildChunkMesh(World *WorldVal, Chunk *ChunkVal);
void UnloadChunkMesh(Chunk *ChunkVal);
void DrawWorld(World *WorldVal, RenderCamera CameraVal);
void DrawAABBDebug(World *WorldVal, Player *PlayerVal);
bool IsChunkInFrustum(RenderCamera CameraVal, Chunk *ChunkVal);

#endif
