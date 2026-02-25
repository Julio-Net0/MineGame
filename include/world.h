#ifndef WORLD_H
#define WORLD_H

#include "raylib.h"
#include "chunk.h"

#define BLOCK_SIZE 1.0F
#define BLOCK_HALF_SIZE (BLOCK_SIZE / 2.0F)

#define MAX_ACTIVE_CHUNKS 9

typedef struct World {
  Chunk chunks[MAX_ACTIVE_CHUNKS];
  int chunkCount;
} World ;

void InitWorld(World *world);
void UpdateWorld(World *world, Vector3 worldPos);
void DrawWorld(World *world);

typedef struct {
  bool hit;
  Vector3 blockPos;
  Vector3 normal;
  int blockID;
} RaycastResult;

int GetBlockIDFromWorld(World *world, Vector3 globalPos);
void SetBlockInWorld(World *world, Vector3 globalPos, unsigned char blockID);
RaycastResult RayCastToWorld(World *world, Vector3 rayOrigin, Vector3 rayDir, float maxDistance);

#endif
