#ifndef RENDERER_H
#define RENDERER_H

#include "raylib.h"
#include "world.h"

typedef enum {
  FACE_TOP,
  FACE_BOTTOM,
  FACE_LEFT,
  FACE_RIGHT,
  FACE_FRONT,
  FACE_BACK
} BlockFace;

void InitRenderer(void);
void BuildChunkMesh(World *world, Chunk *chunk);
void RenderChunkMesh(Chunk *chunk);
void UnloadChunkMesh(Chunk *chunk);
void DrawBlockHighlight(Vector3 pos);

#endif
