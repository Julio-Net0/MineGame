#ifndef RENDERER_H
#define RENDERER_H

#include "raylib.h"
#include "player/player.h"
#include "world/world.h"
#include "rlgl.h"

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
void RenderChunkMesh(Chunk *ChunkVal);
void RenderChunkTranslucentMesh(Chunk *ChunkVal);
void UnloadChunkMesh(Chunk *ChunkVal);
void DrawWorld(World *WorldVal, Camera3D CameraVal);
void DrawBlockHighlight(Vec3 Pos);
void DrawAABBDebug(World *WorldVal, Player *PlayerVal);
void DrawBlockIcon(int BlockId, int X, int Y, int Size);
bool IsChunkInFrustum(Camera3D CameraVal, Chunk *ChunkVal);

#endif
