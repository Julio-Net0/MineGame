#ifndef WORLD_H
#define WORLD_H

#include "core/vecmath.h"
#include "world/chunk.h"
#include "world/dense_pool.h"
#include "world/world_limits.h"
#include <stdbool.h>

// Open-addressing hash map: ~25% load factor keeps probe chains short
#define CHUNK_MAP_SIZE (MAX_ACTIVE_CHUNKS * 4)

typedef void (*ChunkEvictCallback)(Chunk *EvictedChunk);

// Invoked by SetBlockInWorld after a loaded block's ID actually changes.
// Terrain generation writes chunks directly and never triggers it.
typedef void (*BlockChangeCallback)(void *UserData, int BlockX, int BlockY,
                                    int BlockZ, BlockID NewBlockId);

typedef struct World {
  Chunk Chunks[MAX_ACTIVE_CHUNKS];
  int ChunkHashMap[CHUNK_MAP_SIZE];
  // High-water mark of slots ever used, not the number in use: slots below it
  // may be free. Loops over [0, ChunkCount) must skip slots whose SlotInUse is
  // false, since a free slot keeps its last chunk's stale contents.
  int ChunkCount;
  bool SlotInUse[MAX_ACTIVE_CHUNKS];
  int FreeList[MAX_ACTIVE_CHUNKS];
  int FreeCount;

  // Owns the block buffers of every slot that is not uniform. Slots point back
  // at it through Chunk.Pool.
  DenseBlockPool DensePool;

  int LastLoadChunkX;
  int LastLoadChunkY;
  int LastLoadChunkZ;
  int LastLoadRenderDist;
  bool HasLoadedOnce;

  // Inclusive chunk-Y range holding generated content (bedrock band up to the
  // generation ceiling), resolved from the strata table in InitWorld.
  int WorldLoChunkY;
  int WorldHiChunkY;

  ChunkEvictCallback OnChunkEvict;
  BlockChangeCallback OnBlockChange;
  void *BlockChangeUserData;
} World;

typedef struct {
  Vec3 BlockPos;
  Vec3 Normal;
  int BlockId;
  bool Hit;
} RaycastResult;

void InitWorld(World *WorldVal);
// Returns every leased buffer and frees the pool's slabs. The World itself is
// the caller's to free.
void DestroyWorld(World *WorldVal);
void WorldSetChunkEvictCallback(World *WorldVal, ChunkEvictCallback Callback);
void WorldSetBlockChangeCallback(World *WorldVal, BlockChangeCallback Callback,
                                 void *UserData);
// Clamps a requested render distance to [MIN_RENDER_DISTANCE,
// MAX_RENDER_DISTANCE].
int ClampRenderDistance(int RenderDist);

// Number of slots currently holding a chunk.
int WorldActiveChunkCount(const World *WorldVal);

// Leases a dense block buffer in O(1). Returns NULL when the pool is exhausted.
// The buffer's contents are unspecified; the caller fills every cell.
DenseBlockBuffer *WorldAcquireDenseBuffer(World *WorldVal);
// Returns a buffer obtained from WorldAcquireDenseBuffer to the pool in O(1).
void WorldReleaseDenseBuffer(World *WorldVal, DenseBlockBuffer *Buffer);
void WorldGetDensePoolStats(World *WorldVal, DensePoolStats *Out);

typedef struct {
  int UniformAir;
  int UniformSolid;
  int Dense;
} ChunkStorageCounts;

// Classifies every active chunk by its storage: uniform air, uniform non-air,
// or dense (holding a buffer, which includes chunks still generating).
void WorldGetChunkStorageCounts(const World *WorldVal, ChunkStorageCounts *Out);

void UpdateWorld(World *WorldVal, Vec3 PlayerPos, int RenderDist);
void UpdateNeighborsDirtyFlag(World *WorldVal, int Cx, int Cy, int Cz);
int GetBlockIDFromWorld(World *WorldVal, Vec3 GlobalPos);

// Biome id of a global biome cell (cell C spans blocks [C*BIOME_CELL_SIZE ..
// +BIOME_CELL_SIZE-1]). When the owning chunk is not loaded, returns Fallback ???
// the caller's own cell ??? rather than a default biome, so tint interpolation at
// the edge of the loaded world degrades to the local color instead of a seam.
unsigned char GetBiomeCellFromWorld(World *WorldVal, int CellX, int CellY,
                                    int CellZ, unsigned char Fallback);
void SetBlockInWorld(World *WorldVal, Vec3 GlobalPos, BlockID BlockId);
RaycastResult RayCastToWorld(World *WorldVal, Vec3 RayOrigin, Vec3 RayDir, float MaxDistance);
Chunk *GetChunkFromWorld(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ);
Chunk *GetChunkAtPos(World *WorldVal, Vec3 Pos);
bool AreNeighborsGenerated(World *WorldVal, Chunk *ChunkVal);

// True when every chunk position within RenderDist around Center exists and
// has finished background terrain/feature generation (with no pending
// neighbour-dirty bookkeeping). Headless servers query this directly because
// no mesher runs to clear IsDirty.
bool WorldIsGenerationSettled(World *WorldVal, Vec3 Center, int RenderDist);

// Lowest and highest active chunk layer of the load volume UpdateWorld builds
// for Center and RenderDist, and how many layers it holds. Count can be smaller
// than Hi - Lo + 1 when the layers around the centre lie outside the world.
typedef struct {
  int Lo;
  int Hi;
  int Count;
} LoadLayerRange;

void WorldGetLoadLayerRange(const World *WorldVal, Vec3 Center, int RenderDist,
                            LoadLayerRange *Out);

#ifdef MINEGAME_PROFILE
// Sweeps the load volume over chunk Y -40..70 and every render distance and
// checks its invariants (layer budget, the centre and its neighbours present,
// reach, ordering) plus the spec's worked windows. Logs the outcome.
bool WorldCheckLoadVolumes(const World *WorldVal);
#endif

// True when chunk loading around Center has nothing left to do: every position
// within RenderDist exists, has finished generating, and is not waiting to be
// meshed. It reports only what the world itself knows; whether the meshes those
// chunks produced have reached the GPU is the renderer's to answer.
bool WorldIsSettled(World *WorldVal, Vec3 Center, int RenderDist);

// How many active chunks are generated, and how many of those are still waiting
// on generation or meshing. Any out parameter may be null.
void WorldGetLoadCounts(World *WorldVal, int *OutGenerated, int *OutGenerating,
                        int *OutDirty, int *OutMeshing);

#endif
