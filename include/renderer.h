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

void DrawBlockFace(Vector3 pos, Color color, BlockFace face);

void DrawCubeCulled(Chunk *chunk, int x, int y, int z, Color color);

void DrawBlockHighlight(Vector3 pos);

#endif
