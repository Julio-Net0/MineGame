#ifndef WORLD_H
#define WORLD_H

#include "core/vecmath.h"
#include "world/chunk.h"
#include <stdbool.h>

#define MAX_RENDER_DISTANCE 5
#define MAX_ACTIVE_CHUNKS 1331 //Recommended size: (2 * MAX_RENDER_DISTANCE + 1)^3
// Open-addressing hash map: ~25% load factor keeps probe chains short
#define CHUNK_MAP_SIZE (MAX_ACTIVE_CHUNKS * 4)

typedef struct World {
  Chunk Chunks[MAX_ACTIVE_CHUNKS];
  int ChunkHashMap[CHUNK_MAP_SIZE];
  int ChunkCount;
  int FreeList[MAX_ACTIVE_CHUNKS];
  int FreeCount;

  int LastLoadChunkX;
  int LastLoadChunkY;
  int LastLoadChunkZ;
  int LastLoadRenderDist;
  bool HasLoadedOnce;
} World;

typedef struct {
  Vec3 BlockPos;
  Vec3 Normal;
  int BlockId;
  bool Hit;
} RaycastResult;

void InitWorld(World *WorldVal);
void UpdateWorld(World *WorldVal, Vec3 PlayerPos, int RenderDist);
void UpdateNeighborsDirtyFlag(World *WorldVal, int Cx, int Cy, int Cz);
int GetBlockIDFromWorld(World *WorldVal, Vec3 GlobalPos);

// Biome id of a global biome cell (cell C spans blocks [C*BIOME_CELL_SIZE ..
// +BIOME_CELL_SIZE-1]). When the owning chunk is not loaded, returns Fallback —
// the caller's own cell — rather than a default biome, so tint interpolation at
// the edge of the loaded world degrades to the local color instead of a seam.
unsigned char GetBiomeCellFromWorld(World *WorldVal, int CellX, int CellY,
                                    int CellZ, unsigned char Fallback);
void SetBlockInWorld(World *WorldVal, Vec3 GlobalPos, unsigned char BlockId);
RaycastResult RayCastToWorld(World *WorldVal, Vec3 RayOrigin, Vec3 RayDir, float MaxDistance);
Chunk *GetChunkFromWorld(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ);
Chunk *GetChunkAtPos(World *WorldVal, Vec3 Pos);
bool AreNeighborsGenerated(World *WorldVal, Chunk *ChunkVal);

#endif
