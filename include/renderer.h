#ifndef RENDERER_H
#define RENDERER_H

#include "raylib.h"
#include "player.h"
#include "world.h"
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
void BuildChunkMesh(World *world, Chunk *chunk);
void RenderChunkMesh(Chunk *chunk);
void RenderChunkTranslucentMesh(Chunk *chunk);
void UnloadChunkMesh(Chunk *chunk);
void DrawWorld(World *world, Camera3D camera);
void DrawBlockHighlight(Vector3 pos);
void DrawAABBDebug(World *world, Player *player);
void DrawBlockIcon(int blockID, int x, int y, int size);
bool IsChunkInFrustum(Camera3D camera, Chunk *chunk);

#endif
