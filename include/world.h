#ifndef WORLD_H
#define WORLD_H

#include "raylib.h"
#include "chunk.h"

#define MAX_RENDER_DISTANCE 5
#define MAX_ACTIVE_CHUNKS 1331 //Recommended size: (2 * MAX_RENDER_DISTANCE + 1)^3
// Open-addressing hash map: ~25% load factor keeps probe chains short
#define CHUNK_MAP_SIZE (MAX_ACTIVE_CHUNKS * 4)

typedef struct World {
  Chunk chunks[MAX_ACTIVE_CHUNKS];
  int chunkHashMap[CHUNK_MAP_SIZE];
  int chunkCount;
} World ;

typedef struct {
  Vector3 blockPos;
  Vector3 normal;
  int blockID;
  bool hit;
} RaycastResult;

void InitWorld(World *world);
void UpdateWorld(World *world, Vector3 playerPos, int renderDist);
void UpdateNeighborsDirtyFlag(World *world, int cx, int cy, int cz);
int GetBlockIDFromWorld(World *world, Vector3 globalPos);
void SetBlockInWorld(World *world, Vector3 globalPos, unsigned char blockID);
RaycastResult RayCastToWorld(World *world, Vector3 rayOrigin, Vector3 rayDir, float maxDistance);
Chunk* GetChunkFromWorld(World *world, int chunkX, int chunkY, int chunkZ);
Chunk* GetChunkAtPos(World *world, Vector3 pos);
bool AreNeighborsGenerated(World *world, Chunk *chunk);

#endif
