#ifndef WORLD_H
#define WORLD_H

#include "core/vecmath.h"
#include "world/chunk.h"
#include <stdbool.h>

// The active set is a horizontal disc of columns (dx^2 + dz^2 <= R^2) times a
// vertical window of at most 2 * VERTICAL_RENDER_RADIUS + 1 layers around the
// load centre, clamped to the world's generated range. Capacity therefore grows
// with R^2 rather than R^3.
#define MIN_RENDER_DISTANCE 2
#define DEFAULT_RENDER_DISTANCE 8
#define MAX_RENDER_DISTANCE 16
#define VERTICAL_RENDER_RADIUS 8
// Farthest an active layer can lie from the load centre's layer. The window
// holds 2 * VERTICAL_RENDER_RADIUS + 1 layers and always contains the layer
// above the centre, so at most 2 * VERTICAL_RENDER_RADIUS - 1 lie below it.
#define MAX_LOAD_LAYER_REACH ((2 * VERTICAL_RENDER_RADIUS) - 1)

// Lattice points with dx^2 + dz^2 <= MAX_RENDER_DISTANCE^2. A literal because
// C17 cannot count them at compile time; the column table builder never
// exceeds it.
#define MAX_LOADED_COLUMNS 797
#define MAX_ACTIVE_CHUNKS \
  (MAX_LOADED_COLUMNS * ((2 * VERTICAL_RENDER_RADIUS) + 1))
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
void WorldSetChunkEvictCallback(World *WorldVal, ChunkEvictCallback Callback);
void WorldSetBlockChangeCallback(World *WorldVal, BlockChangeCallback Callback,
                                 void *UserData);
// Clamps a requested render distance to [MIN_RENDER_DISTANCE,
// MAX_RENDER_DISTANCE].
int ClampRenderDistance(int RenderDist);

// Number of slots currently holding a chunk.
int WorldActiveChunkCount(const World *WorldVal);

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
