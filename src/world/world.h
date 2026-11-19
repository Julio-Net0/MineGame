#ifndef WORLD_H
#define WORLD_H

#include "core/vecmath.h"
#include "world/chunk.h"
#include "world/column_bounds.h"
#include "world/dense_pool.h"
#include "world/world_limits.h"
#include <stdbool.h>
#include <stdint.h>

// Open-addressing hash map: ~25% load factor keeps probe chains short
#define CHUNK_MAP_SIZE (MAX_ACTIVE_CHUNKS * 4)

// Distance buckets of the mesh-candidate set: one per whole chunk of distance
// from the load centre, the vertical offset counting a quarter in squared
// distance, plus room for chunks a deferred eviction leaves outside the disc.
// Two tiers of them: the near cylinder and every column's surface first, then
// the rock under the far surfaces.
#define MESH_CANDIDATE_TIER_BUCKETS \
  (MAX_RENDER_DISTANCE + (MAX_VERTICAL_LOAD_REACH / 2) + 2)
#define MESH_CANDIDATE_BUCKETS (2 * MESH_CANDIDATE_TIER_BUCKETS)

typedef void (*ChunkEvictCallback)(Chunk *EvictedChunk);

// The layers one column holds, one bit per layer from World.LoadMaskBase.
#define LOAD_LAYER_MASK_WORDS 2
typedef struct {
  uint64_t Bits[LOAD_LAYER_MASK_WORDS];
} LoadLayerMask;

// Open-addressing table of the columns holding loaded chunks. Never more
// columns than chunks, so the table stays at most MAX_ACTIVE_CHUNKS / SIZE full.
#define LOADED_COLUMN_HASH_SIZE 1048576
#define LOADED_COLUMN_HASH_MASK (LOADED_COLUMN_HASH_SIZE - 1)

// One column with loaded chunks, kept from the chunk map alone (insertions and
// removals), so a client holding only the chunks it was sent derives the same
// thing as the world that loaded them.
typedef struct {
  int ColumnX;
  int ColumnZ;
  // Highest loaded layer, exact unless TopStale (then only ever too high, and
  // rescanned on the next read).
  int16_t Top;
  int16_t Count;
  bool Used;
  bool TopStale;
} LoadedColumn;

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
  // Every position of the last load request got a slot (HasLoadedOnce also
  // demands that no eviction was deferred).
  bool LastLoadAllCreated;

  // What the last complete load holds, per column of its volume in volume
  // order: exactly the loaded set, so a load one chunk away diffs against it
  // instead of looking up every position. Only valid while LoadMaskValid; a
  // load that leaves anything uncreated or unevicted drops it.
  LoadLayerMask LoadMasks[MAX_LOADED_COLUMNS];
  int LoadMaskBase;
  int LoadMaskCenterX;
  int LoadMaskCenterY;
  int LoadMaskCenterZ;
  int LoadMaskRadius;
  int LoadMaskColumnCount;
  uint32_t LoadMaskRaisedVersion;
  bool LoadMaskValid;

  // Chunks queued for generation whose completion the main thread has not yet
  // processed. Main thread only.
  int PendingGeneration;
#ifdef MINEGAME_PROFILE
  // Last answers of the counter-based settle checks, so the walks that verify
  // them run once per transition to settled rather than every frame.
  bool LastGenerationSettled;
  bool LastSettled;
#endif

  // Mesh-candidate set: every generated chunk that is dirty or has a mesh in
  // flight, so mesh queueing visits those instead of every slot. Intrusive
  // doubly linked lists per distance bucket, indexed by slot; CandBucket is -1
  // for a slot outside the set. Membership is "dirty or meshing" and is
  // dropped lazily, when the queueing pass finds the chunk clean and idle.
  // Main thread only.
  int CandNext[MAX_ACTIVE_CHUNKS];
  int CandPrev[MAX_ACTIVE_CHUNKS];
  int16_t CandBucket[MAX_ACTIVE_CHUNKS];
  int CandHead[MESH_CANDIDATE_BUCKETS];
  int CandidateCount;
  // The load centre the buckets were computed against.
  int CandCenterX;
  int CandCenterY;
  int CandCenterZ;

  // Inclusive chunk-Y range holding generated content (bedrock band up to the
  // generation ceiling), resolved from the strata table in InitWorld.
  int WorldLoChunkY;
  int WorldHiChunkY;

  // Density bounds of every column around the load centre, refreshed by
  // UpdateWorld whenever the centre moves. Main thread only.
  ColumnBoundsTable ColumnBounds;
  LoadedColumn LoadedColumns[LOADED_COLUMN_HASH_SIZE];
  // Columns holding player edits above their terrain bounds, seeded from the
  // save at start-up. Main thread only.
  RaisedTopMap RaisedTops;

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
  int Packed;
  int Dense;
} ChunkStorageCounts;

// Classifies every active chunk by its storage: uniform air, uniform non-air,
// packed (a palette of at most 16 ids), or dense (a full buffer, which includes
// chunks still generating).
void WorldGetChunkStorageCounts(const World *WorldVal, ChunkStorageCounts *Out);

void UpdateWorld(World *WorldVal, Vec3 PlayerPos, int RenderDist);
void UpdateNeighborsDirtyFlag(World *WorldVal, int Cx, int Cy, int Cz);

// Tick step: reacts to the chunks the workers finished generating since the
// last call by marking their neighbours dirty. Costs nothing when none did.
void WorldProcessGenerationEvents(World *WorldVal);

// Mesh-candidate iteration, nearest bucket first. Pass -1 to get the first
// candidate; returns -1 past the last. Fetch the next slot before removing the
// current one.
int WorldNextMeshCandidate(const World *WorldVal, int Slot);
// Drops a slot from the candidate set; a no-op for a slot outside it. Callers
// remove a chunk only once it is neither dirty nor meshing.
void WorldRemoveMeshCandidate(World *WorldVal, int Slot);
int WorldMeshCandidateCount(const World *WorldVal);
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

// The vertical window the near columns of the load volume for Center and
// RenderDist hold around the centre: lowest and highest layer, and how many.
// Count can be smaller than Hi - Lo + 1 when the layers around the centre lie
// outside the world.
typedef struct {
  int Lo;
  int Hi;
  int Count;
} LoadLayerRange;

void WorldGetLoadLayerRange(World *WorldVal, Vec3 Center, int RenderDist,
                            LoadLayerRange *Out);

// Size of the load volume for Center and RenderDist.
typedef struct {
  int Columns;
  int NearColumns;
  // Columns with a non-empty band, and the highest band layer among them.
  int BandColumns;
  int MaxLayer;
  // Positions the volume holds: what chunks.active settles at.
  int Chunks;
  // Far columns the slot budget cut down to their top layer.
  int TrimmedColumns;
  // Columns anywhere in the world with player edits recorded above their
  // terrain bounds.
  int RaisedColumns;
} LoadVolumeStats;

void WorldGetLoadVolumeStats(World *WorldVal, Vec3 Center, int RenderDist,
                             LoadVolumeStats *Out);

// The highest loaded layer of column (ColumnX, ColumnZ); false when the
// column holds no loaded chunk.
bool WorldGetLoadedColumnTop(World *WorldVal, int ColumnX, int ColumnZ,
                             int *OutTop);

// For a chunk position absent from the world, in a column holding loaded
// chunks: rock when it lies below the column's highest loaded layer and inside
// the world (below the terrain band, or between band and window), sky when it
// lies above that layer or the world. In a column holding nothing it is
// neither. Read from the loaded chunks alone, never from the seed. Readers
// that need what lies there use these instead of assuming air.
bool WorldIsUnloadedRock(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ);
bool WorldIsUnloadedSky(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ);

// Which of the six face neighbours of chunk position (ChunkX, ChunkY, ChunkZ)
// are absent and unloaded rock: bit 2 * axis + (1 for the positive side), axes
// X, Y, Z. The rock there has faces toward any open cell beside it, which no
// loaded chunk draws.
unsigned WorldUnloadedRockFaces(World *WorldVal, int ChunkX, int ChunkY,
                                int ChunkZ);

#ifdef MINEGAME_PROFILE
// Sweeps the load volume over chunk Y -40..70 and every render distance and
// checks the window's invariants (layer budget, the centre and its neighbours
// present, reach, ordering), the bands' at a few radii (inside the world, no
// duplicate layer, within the slot pool), and the spec's worked windows. Logs
// the outcome.
bool WorldCheckLoadVolumes(World *WorldVal);
#endif

// True when chunk loading around Center has nothing left to do: every position
// within RenderDist exists, has finished generating, and is not waiting to be
// meshed. It reports only what the world itself knows; whether the meshes those
// chunks produced have reached the GPU is the renderer's to answer.
bool WorldIsSettled(World *WorldVal, Vec3 Center, int RenderDist);

// True when every loaded position within horizontal distance Radius (in chunks)
// of Center is present, generated, and neither dirty nor meshing, for the load
// request made at Center and RenderDist. Costs time proportional to the chunks
// within Radius, not to the whole volume.
bool WorldIsNearSettled(World *WorldVal, Vec3 Center, int RenderDist,
                        int Radius);

// True when the top WORLD_SURFACE_SETTLE_LAYERS layers of every column's band,
// the terrain surface and horizon seen from afar, are present, generated, and
// neither dirty nor meshing, for the load request made at Center and
// RenderDist. Costs one lookup per checked layer of every loaded column.
#define WORLD_SURFACE_SETTLE_LAYERS 2
bool WorldIsSurfaceSettled(World *WorldVal, Vec3 Center, int RenderDist);

// The load disc's terrain-guided vertical extent against the current window:
// what a per-column load range would hold, and what the uniform window cuts.
// Columns outside the disc are not counted. Layers are clamped to the world.
typedef struct {
  int Columns;
  // Columns times the window's layer span.
  int WindowChunks;
  // Sum over columns of TopLayer - BottomLayer + 1.
  int BandChunks;
  // Columns whose top lies above the window, and the layers they lose there.
  int SlicedColumns;
  int SlicedLayers;
  // Columns whose terrain air reaches below the window.
  int UncoveredBelowColumns;
  // Window layers above a column's top: provably air.
  int AirWindowChunks;
  int MaxTopLayer;
  int MinBottomLayer;
  int ReachColumns;
  int ReachAbove;
  uint64_t Computed;
} ColumnBoundsStats;

void WorldGetColumnBoundsStats(World *WorldVal, Vec3 Center, int RenderDist,
                               ColumnBoundsStats *Out);

// How many active chunks are generated, and how many of those are still waiting
// on generation or meshing. Any out parameter may be null.
void WorldGetLoadCounts(World *WorldVal, int *OutGenerated, int *OutGenerating,
                        int *OutDirty, int *OutMeshing);

#endif
