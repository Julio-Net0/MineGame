#include "world/world.h"
#include "world/chunk.h"
#include "world/chunk_worker.h"
#include "persistence/world_save.h"
#include "core/profiler.h"
#include "core/log.h"
#include "world/worldgen_strata.h"
#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DDA_MAX_CROSSED_AXES 3.0F

#define HASH_EMPTY (-1)

#define SPATIAL_PRIME_X (73856093)
#define SPATIAL_PRIME_Y (19349663)
#define SPATIAL_PRIME_Z (83492791)
 
typedef struct {
  float TMaxX, TMaxY, TMaxZ;
  float TDeltaX, TDeltaY, TDeltaZ;
  float DistanceTravelled;

  int VoxelX, VoxelY, VoxelZ;
  int StepX, StepY, StepZ;
  int LastStep;
} DdaState;

enum {
  COLUMN_CANDIDATE_SIDE = (2 * MAX_RENDER_DISTANCE) + 1,
  MAX_LOAD_LAYERS = (2 * VERTICAL_RENDER_RADIUS) + 1
};

typedef struct {
  int Dx;
  int Dz;
} ColumnOffset;

// Every chunk position the active set covers around one centre: column offsets
// nearest first, and layers ordered by distance from the centre layer.
typedef struct {
  int CenterX;
  int CenterZ;
  const ColumnOffset *Columns;
  int ColumnCount;
  int Layers[MAX_LOAD_LAYERS];
  int LayerCount;
} LoadVolume;

static int HashChunkPos(int Cx, int Cy, int Cz){
  unsigned int H = (unsigned int)((Cx * SPATIAL_PRIME_X) ^ (Cy * SPATIAL_PRIME_Y) ^ (Cz * SPATIAL_PRIME_Z));
  return (int)(H % (unsigned int)CHUNK_MAP_SIZE);
}

static int FloorDivChunkSize(int Value) {
  int Quotient = Value / CHUNK_SIZE;
  if ((Value % CHUNK_SIZE) != 0 && Value < 0) {
    Quotient--;
  }
  return Quotient;
}

static void ResolveWorldChunkRange(int *OutLo, int *OutHi) {
  int MinY = INT_MIN;
  int MaxY = INT_MAX;
  if (!GetStratumRange(STRATUM_GEN_BEDROCK_BAND, &MinY, &MaxY) &&
      !GetStratumRange(STRATUM_GEN_OVERWORLD, &MinY, &MaxY)) {
    MinY = INT_MIN;
  }
  *OutLo = FloorDivChunkSize(MinY);
  *OutHi = FloorDivChunkSize(GetGenerationCeiling());
}

int ClampRenderDistance(int RenderDist) {
  if (RenderDist < MIN_RENDER_DISTANCE) { return MIN_RENDER_DISTANCE; }
  if (RenderDist > MAX_RENDER_DISTANCE) { return MAX_RENDER_DISTANCE; }
  return RenderDist;
}

int WorldActiveChunkCount(const World *WorldVal) {
  return MAX_ACTIVE_CHUNKS - WorldVal->FreeCount;
}

static int CompareColumnOffsets(const void *Lhs, const void *Rhs) {
  const ColumnOffset *A = (const ColumnOffset *)Lhs;
  const ColumnOffset *B = (const ColumnOffset *)Rhs;
  int DistA = (A->Dx * A->Dx) + (A->Dz * A->Dz);
  int DistB = (B->Dx * B->Dx) + (B->Dz * B->Dz);
  if (DistA != DistB) { return (DistA < DistB) ? -1 : 1; }
  if (A->Dx != B->Dx) { return (A->Dx < B->Dx) ? -1 : 1; }
  if (A->Dz != B->Dz) { return (A->Dz < B->Dz) ? -1 : 1; }
  return 0;
}

// Disc of column offsets for RenderDist, sorted nearest first. Rebuilt only
// when the radius changes; the world is driven from one thread.
static const ColumnOffset *GetColumnOffsets(int RenderDist, int *OutCount) {
  static ColumnOffset Offsets[COLUMN_CANDIDATE_SIDE * COLUMN_CANDIDATE_SIDE];
  static int Count = 0;
  static int CachedRadius = -1;

  if (CachedRadius != RenderDist) {
    int RadiusSq = RenderDist * RenderDist;
    Count = 0;
    for (int Dx = -RenderDist; Dx <= RenderDist; Dx++) {
      for (int Dz = -RenderDist; Dz <= RenderDist; Dz++) {
        if ((Dx * Dx) + (Dz * Dz) <= RadiusSq) {
          Offsets[Count++] = (ColumnOffset){Dx, Dz};
        }
      }
    }
    qsort(Offsets, (size_t)Count, sizeof(Offsets[0]), CompareColumnOffsets);
    // Unreachable while RenderDist is clamped to MAX_RENDER_DISTANCE; dropping
    // the farthest columns keeps the slot pool from ever being oversubscribed.
    if (Count > MAX_LOADED_COLUMNS) {
      Count = MAX_LOADED_COLUMNS;
    }
    CachedRadius = RenderDist;
  }

  *OutCount = Count;
  return Offsets;
}

enum {
  // Underground, each layer the centre sits below the first layer under the
  // sea-level chunk moves this many window layers from above it to below it.
  DEPTH_SHIFT_PER_LAYER = 1,
  // Layers the window keeps above the centre however deep it is, so a cavern's
  // ceiling stays loaded.
  MIN_UP_LAYERS = 2
};

_Static_assert(MAX_LOAD_LAYERS == MAX_LOAD_LAYER_REACH + 2,
               "a window of MAX_LOAD_LAYERS layers holding the layer above the "
               "centre reaches at most MAX_LOAD_LAYER_REACH below it");

// The 2V + 1 layer window around CenterY. Underground it is biased downward,
// where the visible depth is; then it slides off the generation ceiling and the
// bedrock floor instead of being cut there, so layers outside the world are
// spent inside it. Each slide stops one layer short of the centre, so the
// window always holds CenterY - 1 .. CenterY + 1.
static void ComputeLayerWindow(int CenterY, int Vertical, int WorldLo,
                               int WorldHi, int *OutLo, int *OutHi) {
  int Depth = (FloorDivChunkSize(SEA_LEVEL) - 1) - CenterY;
  if (Depth > Vertical) { Depth = Vertical; }
  int Shift = Depth * DEPTH_SHIFT_PER_LAYER;
  if (Shift > Vertical - MIN_UP_LAYERS) { Shift = Vertical - MIN_UP_LAYERS; }
  if (Shift < 0) { Shift = 0; }

  int Lo = CenterY - (Vertical + Shift);
  int Hi = CenterY + (Vertical - Shift);

  if (Hi > WorldHi) {
    int Move = Hi - WorldHi;
    int MaxMove = (Hi - CenterY) - 1;
    if (Move > MaxMove) { Move = MaxMove; }
    Lo -= Move;
    Hi -= Move;
  }
  if (Lo < WorldLo) {
    int Move = WorldLo - Lo;
    int MaxMove = (CenterY - Lo) - 1;
    if (Move > MaxMove) { Move = MaxMove; }
    Lo += Move;
    Hi += Move;
  }

  *OutLo = Lo;
  *OutHi = Hi;
}

// Layers are (the window intersected with the world's range) united with
// [Cy - 1, Cy + 1], so the chunks around the centre stay loaded even outside
// the world. Both parts lie inside the window, which bounds the layer count by
// MAX_LOAD_LAYERS. Layers are listed by distance from the centre, below first.
static void BuildLoadVolume(const World *WorldVal, Vec3 Center, int RenderDist,
                            LoadVolume *Out) {
  int Radius = ClampRenderDistance(RenderDist);
  int Vertical = (Radius < VERTICAL_RENDER_RADIUS) ? Radius
                                                   : VERTICAL_RENDER_RADIUS;
  int CenterY = (int)__builtin_floorf(Center.y / CHUNK_SIZE);

  Out->CenterX = (int)__builtin_floorf(Center.x / CHUNK_SIZE);
  Out->CenterZ = (int)__builtin_floorf(Center.z / CHUNK_SIZE);
  Out->Columns = GetColumnOffsets(Radius, &Out->ColumnCount);

  int Lo = 0;
  int Hi = 0;
  ComputeLayerWindow(CenterY, Vertical, WorldVal->WorldLoChunkY,
                     WorldVal->WorldHiChunkY, &Lo, &Hi);
  if (Lo < WorldVal->WorldLoChunkY) { Lo = WorldVal->WorldLoChunkY; }
  if (Hi > WorldVal->WorldHiChunkY) { Hi = WorldVal->WorldHiChunkY; }

  Out->LayerCount = 0;
  for (int Step = 0; Step <= MAX_LOAD_LAYER_REACH; Step++) {
    int Below = CenterY - Step;
    int Above = CenterY + Step;
    if ((Below >= Lo && Below <= Hi) || Step <= 1) {
      Out->Layers[Out->LayerCount++] = Below;
    }
    if (Step > 0 && ((Above >= Lo && Above <= Hi) || Step <= 1)) {
      Out->Layers[Out->LayerCount++] = Above;
    }
  }
}

#ifdef MINEGAME_PROFILE
static bool LoadVolumeIsValid(const LoadVolume *Volume, int CenterY,
                              int Vertical) {
  if (Volume->LayerCount > (2 * Vertical) + 1) { return false; }
  int NearSeen = 0;
  int PrevDistance = 0;
  for (int Idx = 0; Idx < Volume->LayerCount; Idx++) {
    int Layer = Volume->Layers[Idx];
    int Distance = abs(Layer - CenterY);
    if (Distance > MAX_LOAD_LAYER_REACH || Distance < PrevDistance) {
      return false;
    }
    PrevDistance = Distance;
    for (int Other = 0; Other < Idx; Other++) {
      if (Volume->Layers[Other] == Layer) { return false; }
    }
    if (Distance <= 1) { NearSeen++; }
  }
  return NearSeen == 3;
}

typedef struct {
  int CenterY;
  int Lo;
  int Hi;
} WindowExpectation;

// A point inside chunk (0, CenterY, 0), away from any chunk border.
static Vec3 PointInChunkLayer(int CenterY) {
  return (Vec3){BLOCK_HALF_SIZE,
                ((float)CenterY * CHUNK_SIZE) + BLOCK_HALF_SIZE,
                BLOCK_HALF_SIZE};
}

bool WorldCheckLoadVolumes(const World *WorldVal) {
  enum { SWEEP_LO = -40, SWEEP_HI = 70, SPEC_WORLD_LO = -33, SPEC_WORLD_HI = 40 };
  int Failures = 0;
  LoadVolume Volume;

  for (int Radius = MIN_RENDER_DISTANCE; Radius <= MAX_RENDER_DISTANCE; Radius++) {
    int Vertical = (Radius < VERTICAL_RENDER_RADIUS) ? Radius
                                                     : VERTICAL_RENDER_RADIUS;
    for (int CenterY = SWEEP_LO; CenterY <= SWEEP_HI; CenterY++) {
      BuildLoadVolume(WorldVal, PointInChunkLayer(CenterY), Radius, &Volume);
      if (!LoadVolumeIsValid(&Volume, CenterY, Vertical)) {
        LogError("WORLD: load volume invalid at chunk Y %d, R %d", CenterY,
                 Radius);
        Failures++;
      }
    }
  }

  // The worked values of the cylindrical-chunk-loading spec, which assume the
  // default strata.
  if (WorldVal->WorldLoChunkY == SPEC_WORLD_LO &&
      WorldVal->WorldHiChunkY == SPEC_WORLD_HI) {
    static const WindowExpectation EXPECTED[] = {
        {4, -4, 12},    {-1, -9, 7},  {-8, -22, -6},
        {-32, -33, -17}, {38, 24, 40}, {60, 59, 61},
    };
    for (size_t Idx = 0; Idx < sizeof(EXPECTED) / sizeof(EXPECTED[0]); Idx++) {
      const WindowExpectation *Case = &EXPECTED[Idx];
      LoadLayerRange Range;
      WorldGetLoadLayerRange(WorldVal, PointInChunkLayer(Case->CenterY),
                             DEFAULT_RENDER_DISTANCE, &Range);
      if (Range.Lo != Case->Lo || Range.Hi != Case->Hi) {
        LogError("WORLD: chunk Y %d loads %d..%d, expected %d..%d",
                 Case->CenterY, Range.Lo, Range.Hi, Case->Lo, Case->Hi);
        Failures++;
      }
    }
  }

  if (Failures == 0) {
    LogInfo("WORLD: load volumes valid for chunk Y %d..%d, R %d..%d",
            SWEEP_LO, SWEEP_HI, MIN_RENDER_DISTANCE, MAX_RENDER_DISTANCE);
  }
  return Failures == 0;
}
#endif

void InitWorld(World *WorldVal){
  WorldVal->ChunkCount = 0;

  for(int IdxI = 0; IdxI < CHUNK_MAP_SIZE; IdxI++){
    WorldVal->ChunkHashMap[IdxI] = HASH_EMPTY;
  }

  // Stacked so the lowest index pops first: ChunkCount then tracks how many
  // slots the chosen render distance actually needs, not the whole pool.
  WorldVal->FreeCount = MAX_ACTIVE_CHUNKS;
  for(int IdxI = 0; IdxI < MAX_ACTIVE_CHUNKS; IdxI++){
    WorldVal->FreeList[IdxI] = MAX_ACTIVE_CHUNKS - 1 - IdxI;
    WorldVal->SlotInUse[IdxI] = false;
  }

  WorldVal->LastLoadChunkX = 0;
  WorldVal->LastLoadChunkY = 0;
  WorldVal->LastLoadChunkZ = 0;
  WorldVal->LastLoadRenderDist = 0;
  WorldVal->HasLoadedOnce = false;
  ResolveWorldChunkRange(&WorldVal->WorldLoChunkY, &WorldVal->WorldHiChunkY);
  WorldVal->OnChunkEvict = (ChunkEvictCallback)0;
  WorldVal->OnBlockChange = (BlockChangeCallback)0;
  WorldVal->BlockChangeUserData = NULL;
}

void WorldSetChunkEvictCallback(World *WorldVal, ChunkEvictCallback Callback) {
  WorldVal->OnChunkEvict = Callback;
}

void WorldSetBlockChangeCallback(World *WorldVal, BlockChangeCallback Callback,
                                 void *UserData) {
  WorldVal->OnBlockChange = Callback;
  WorldVal->BlockChangeUserData = UserData;
}

static int HashChunkSlot(const World *WorldVal, int ChunkIndex) {
  const Chunk *ChunkVal = &WorldVal->Chunks[ChunkIndex];
  return HashChunkPos(ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
}

static bool ChunkIsAt(const World *WorldVal, int ChunkIndex, int ChunkX,
                      int ChunkY, int ChunkZ) {
  const Chunk *ChunkVal = &WorldVal->Chunks[ChunkIndex];
  return ChunkVal->ChunkX == ChunkX && ChunkVal->ChunkY == ChunkY &&
         ChunkVal->ChunkZ == ChunkZ;
}

// Cyclic number of probe steps from slot From forward to slot To.
static int ProbeDistance(int From, int To) {
  return (To - From + CHUNK_MAP_SIZE) % CHUNK_MAP_SIZE;
}

// The map is at most 25% full (CHUNK_MAP_SIZE = 4 * MAX_ACTIVE_CHUNKS), so
// every probe sequence reaches an empty slot and these loops terminate.
static void InsertChunkIntoMap(World *WorldVal, int ChunkIndex) {
  int Hash = HashChunkSlot(WorldVal, ChunkIndex);

  while(WorldVal->ChunkHashMap[Hash] != HASH_EMPTY){
    Hash = (Hash + 1) % CHUNK_MAP_SIZE;
  }
  WorldVal->ChunkHashMap[Hash] = ChunkIndex;
}

// Backward-shift deletion: rather than leaving a tombstone, entries after the
// hole that probed past it are moved back into it, so the table ends up exactly
// as if the removed chunk had never been inserted. An entry may fill the hole
// only when the hole lies on its probe path, i.e. between its home slot and its
// current slot (cyclically); otherwise moving it would put it before its home
// and lookups starting there would miss it.
static void RemoveChunkFromMap(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ) {
  int Hole = HashChunkPos(ChunkX, ChunkY, ChunkZ);

  for(;;) {
    int Idx = WorldVal->ChunkHashMap[Hole];
    if(Idx == HASH_EMPTY) { return; }
    if(ChunkIsAt(WorldVal, Idx, ChunkX, ChunkY, ChunkZ)) { break; }
    Hole = (Hole + 1) % CHUNK_MAP_SIZE;
  }

  int Next = (Hole + 1) % CHUNK_MAP_SIZE;
  while(WorldVal->ChunkHashMap[Next] != HASH_EMPTY) {
    int Idx = WorldVal->ChunkHashMap[Next];
    int Home = HashChunkSlot(WorldVal, Idx);
    if(ProbeDistance(Home, Next) >= ProbeDistance(Hole, Next)) {
      WorldVal->ChunkHashMap[Hole] = Idx;
      Hole = Next;
    }
    Next = (Next + 1) % CHUNK_MAP_SIZE;
  }
  WorldVal->ChunkHashMap[Hole] = HASH_EMPTY;
}

Chunk* GetChunkFromWorld(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ){
  int Hash = HashChunkPos(ChunkX, ChunkY, ChunkZ);

  while(WorldVal->ChunkHashMap[Hash] != HASH_EMPTY) {
    int Idx = WorldVal->ChunkHashMap[Hash];
    if(ChunkIsAt(WorldVal, Idx, ChunkX, ChunkY, ChunkZ)) {
      return &WorldVal->Chunks[Idx];
    }
    Hash = (Hash + 1) % CHUNK_MAP_SIZE;
  }
  return (Chunk *)0;
}

Chunk* GetChunkAtPos(World *WorldVal, Vec3 Pos){
  int ChunkX = (int)__builtin_floorf(Pos.x / CHUNK_SIZE);
  int ChunkY = (int)__builtin_floorf(Pos.y / CHUNK_SIZE);
  int ChunkZ = (int)__builtin_floorf(Pos.z / CHUNK_SIZE);

  return GetChunkFromWorld(WorldVal, ChunkX, ChunkY, ChunkZ);
}

static void MarkUsefulChunks(World *WorldVal, const LoadVolume *Volume, bool *KeepChunk){
  for(int Col = 0; Col < Volume->ColumnCount; Col++){
    int ChunkX = Volume->CenterX + Volume->Columns[Col].Dx;
    int ChunkZ = Volume->CenterZ + Volume->Columns[Col].Dz;
    for(int Layer = 0; Layer < Volume->LayerCount; Layer++){
      Chunk* ChunkVal = GetChunkFromWorld(WorldVal, ChunkX, Volume->Layers[Layer], ChunkZ);
      if(ChunkVal != (Chunk *)0){
        KeepChunk[ChunkVal - WorldVal->Chunks] = true;
      }
    }
  }
}

void UpdateNeighborsDirtyFlag(World *WorldVal, int Cx, int Cy, int Cz){
  Chunk *Neighbor;
  Neighbor = GetChunkFromWorld(WorldVal, Cx - 1, Cy, Cz); if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  Neighbor = GetChunkFromWorld(WorldVal, Cx + 1, Cy, Cz); if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  Neighbor = GetChunkFromWorld(WorldVal, Cx, Cy - 1, Cz); if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  Neighbor = GetChunkFromWorld(WorldVal, Cx, Cy + 1, Cz); if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  Neighbor = GetChunkFromWorld(WorldVal, Cx, Cy, Cz - 1); if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  Neighbor = GetChunkFromWorld(WorldVal, Cx, Cy, Cz + 1); if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
}

static void EvictUnneededChunks(World *WorldVal, const bool *KeepChunk){
  for(int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++){
    if(!WorldVal->SlotInUse[IdxI] || KeepChunk[IdxI] ||
       WorldVal->Chunks[IdxI].IsGenerating || WorldVal->Chunks[IdxI].IsMeshing){
      continue;
    }

    int OldX = WorldVal->Chunks[IdxI].ChunkX;
    int OldY = WorldVal->Chunks[IdxI].ChunkY;
    int OldZ = WorldVal->Chunks[IdxI].ChunkZ;

    if(WorldVal->Chunks[IdxI].IsModified){
      SaveChunkToDisk(&WorldVal->Chunks[IdxI]);
    }

    Chunk *Evicted = &WorldVal->Chunks[IdxI];
    if(WorldVal->OnChunkEvict != (ChunkEvictCallback)0){
      WorldVal->OnChunkEvict(Evicted);
    }

    RemoveChunkFromMap(WorldVal, OldX, OldY, OldZ);
    UpdateNeighborsDirtyFlag(WorldVal, OldX, OldY, OldZ);

    WorldVal->SlotInUse[IdxI] = false;
    WorldVal->FreeList[WorldVal->FreeCount++] = IdxI;
  }
}

static bool CreateOrRecycleChunk(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ){
  if(WorldVal->FreeCount == 0){
    return false;
  }

  int IdxI = WorldVal->FreeList[--WorldVal->FreeCount];

  // A slot above the high-water mark has never held a chunk; its memory is
  // whatever malloc returned, so it starts from a known all-false state.
  if(IdxI >= WorldVal->ChunkCount){
    memset(&WorldVal->Chunks[IdxI], 0, sizeof(WorldVal->Chunks[IdxI]));
  }
  WorldVal->SlotInUse[IdxI] = true;

  WorldVal->Chunks[IdxI].ChunkX = ChunkX;
  WorldVal->Chunks[IdxI].ChunkY = ChunkY;
  WorldVal->Chunks[IdxI].ChunkZ = ChunkZ;

  EnqueueChunkGeneration(&WorldVal->Chunks[IdxI]);
  InsertChunkIntoMap(WorldVal, IdxI);

  if(IdxI >= WorldVal->ChunkCount){
    WorldVal->ChunkCount = IdxI + 1;
  }
  return true;
}

void UpdateWorld(World *WorldVal, Vec3 PlayerPos, int RenderDist){
  PROFILE_BEGIN(UpdateStart);
  RenderDist = ClampRenderDistance(RenderDist);

  int PChunkX = (int)__builtin_floorf(PlayerPos.x / CHUNK_SIZE);
  int PChunkY = (int)__builtin_floorf(PlayerPos.y / CHUNK_SIZE);
  int PChunkZ = (int)__builtin_floorf(PlayerPos.z / CHUNK_SIZE);

  // Gate: the active-chunk set is a pure function of the player's chunk
  // coordinate and the (clamped) render distance. Skip the recompute when
  // neither changed since the last load. Async generation and meshing run
  // outside this function, so in-flight chunks still complete while stationary.
  if(WorldVal->HasLoadedOnce &&
     WorldVal->LastLoadChunkX == PChunkX &&
     WorldVal->LastLoadChunkY == PChunkY &&
     WorldVal->LastLoadChunkZ == PChunkZ &&
     WorldVal->LastLoadRenderDist == RenderDist){
    PROFILE_END(UpdateStart, PROFILE_WORLD_UPDATE);
    return;
  }

  // Static: at MAX_ACTIVE_CHUNKS entries it is too large to want on the stack,
  // and UpdateWorld only runs on the thread driving the world.
  static bool KeepChunk[MAX_ACTIVE_CHUNKS];
  memset(KeepChunk, 0, sizeof(KeepChunk));

  LoadVolume Volume;
  BuildLoadVolume(WorldVal, PlayerPos, RenderDist, &Volume);

  MarkUsefulChunks(WorldVal, &Volume, KeepChunk);
  EvictUnneededChunks(WorldVal, KeepChunk);

  // Volume order is nearest first, so generation is enqueued nearest first.
  bool AllCreated = true;
  for(int Col = 0; Col < Volume.ColumnCount; Col++){
    int ChunkX = Volume.CenterX + Volume.Columns[Col].Dx;
    int ChunkZ = Volume.CenterZ + Volume.Columns[Col].Dz;
    for(int Layer = 0; Layer < Volume.LayerCount; Layer++){
      int ChunkY = Volume.Layers[Layer];
      if(GetChunkFromWorld(WorldVal, ChunkX, ChunkY, ChunkZ) == (Chunk *)0 &&
         !CreateOrRecycleChunk(WorldVal, ChunkX, ChunkY, ChunkZ)){
        AllCreated = false;
      }
    }
  }

  WorldVal->LastLoadChunkX = PChunkX;
  WorldVal->LastLoadChunkY = PChunkY;
  WorldVal->LastLoadChunkZ = PChunkZ;
  WorldVal->LastLoadRenderDist = RenderDist;
  WorldVal->HasLoadedOnce = AllCreated;
  PROFILE_END(UpdateStart, PROFILE_WORLD_UPDATE);
}

static Chunk* GetLocalCoords(World *WorldVal, Vec3 GlobalPos, int *LocalX, int *LocalY, int *LocalZ){
  int ChunkX = (int)__builtin_floorf(GlobalPos.x / CHUNK_SIZE);
  int ChunkY = (int)__builtin_floorf(GlobalPos.y / CHUNK_SIZE);
  int ChunkZ = (int)__builtin_floorf(GlobalPos.z / CHUNK_SIZE);

  *LocalX = (int)__builtin_floorf(GlobalPos.x) - (ChunkX * CHUNK_SIZE);
  *LocalY = (int)__builtin_floorf(GlobalPos.y) - (ChunkY * CHUNK_SIZE);
  *LocalZ = (int)__builtin_floorf(GlobalPos.z) - (ChunkZ * CHUNK_SIZE);

  return GetChunkFromWorld(WorldVal, ChunkX, ChunkY, ChunkZ);
}

void SetBlockInWorld(World *WorldVal, Vec3 Pos, BlockID BlockId){
  Chunk *ChunkVal = GetChunkAtPos(WorldVal, Pos);
  if (ChunkVal == (Chunk *)0) { return; }

  int LocalX = ((int)__builtin_floorf(Pos.x)) % CHUNK_SIZE;
  int LocalY = ((int)__builtin_floorf(Pos.y)) % CHUNK_SIZE;
  int LocalZ = ((int)__builtin_floorf(Pos.z)) % CHUNK_SIZE;

  if (LocalX < 0) { LocalX += CHUNK_SIZE; }
  if (LocalY < 0) { LocalY += CHUNK_SIZE; }
  if (LocalZ < 0) { LocalZ += CHUNK_SIZE; }

  BlockID PreviousId = ChunkVal->Data[LocalX][LocalY][LocalZ];
  SetBlockInChunk(ChunkVal, (Vec3){(float)LocalX, (float)LocalY, (float)LocalZ}, BlockId);
  ChunkVal->IsDirty = true;
  // A mesh already in flight was built from a snapshot taken before this edit,
  // so it is stale. Completion checks this rather than clearing IsDirty blindly.
  if (ChunkVal->IsMeshing) { ChunkVal->DirtiedWhileMeshing = true; }

  if (LocalX == 0) {
    Chunk *Neighbor = GetChunkFromWorld(WorldVal, ChunkVal->ChunkX - 1, ChunkVal->ChunkY, ChunkVal->ChunkZ);
    if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  } 
  else if (LocalX == CHUNK_SIZE - 1) {
    Chunk *Neighbor = GetChunkFromWorld(WorldVal, ChunkVal->ChunkX + 1, ChunkVal->ChunkY, ChunkVal->ChunkZ);
    if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  }

  if (LocalY == 0) {
    Chunk *Neighbor = GetChunkFromWorld(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY - 1, ChunkVal->ChunkZ);
    if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  } 
  else if (LocalY == CHUNK_SIZE - 1) {
    Chunk *Neighbor = GetChunkFromWorld(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY + 1, ChunkVal->ChunkZ);
    if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  }

  if (LocalZ == 0) {
    Chunk *Neighbor = GetChunkFromWorld(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ - 1);
    if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  } 
  else if (LocalZ == CHUNK_SIZE - 1) {
    Chunk *Neighbor = GetChunkFromWorld(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ + 1);
    if (Neighbor != (Chunk *)0) { Neighbor->IsDirty = true; }
  }

  if (WorldVal->OnBlockChange != (BlockChangeCallback)0 && PreviousId != BlockId) {
    WorldVal->OnBlockChange(WorldVal->BlockChangeUserData,
                            (int)__builtin_floorf(Pos.x),
                            (int)__builtin_floorf(Pos.y),
                            (int)__builtin_floorf(Pos.z), BlockId);
  }
}

int GetBlockIDFromWorld(World *WorldVal, Vec3 GlobalPos){
  int Lx; 
  int Ly; 
  int Lz;

  Chunk *ChunkVal = GetLocalCoords(WorldVal, GlobalPos, &Lx, &Ly, &Lz);

  if(ChunkVal != (Chunk *)0){
    return GetBlockIdInChunk(ChunkVal, (Vec3){(float)Lx, (float)Ly, (float)Lz});
  }

  return 0;
}

// Floor division, so a negative cell index maps to the chunk below rather than
// truncating toward zero and landing in the wrong one.
static int FloorDivCells(int Value, int Divisor) {
  int Quotient = Value / Divisor;
  if ((Value % Divisor != 0) && ((Value < 0) != (Divisor < 0))) {
    Quotient--;
  }
  return Quotient;
}

unsigned char GetBiomeCellFromWorld(World *WorldVal, int CellX, int CellY,
                                    int CellZ, unsigned char Fallback) {
  int ChunkX = FloorDivCells(CellX, BIOME_CELLS_PER_CHUNK);
  int ChunkY = FloorDivCells(CellY, BIOME_CELLS_PER_CHUNK);
  int ChunkZ = FloorDivCells(CellZ, BIOME_CELLS_PER_CHUNK);

  Chunk *ChunkVal = GetChunkFromWorld(WorldVal, ChunkX, ChunkY, ChunkZ);
  if (ChunkVal == (Chunk *)0) {
    return Fallback;
  }

  int LocalX = CellX - (ChunkX * BIOME_CELLS_PER_CHUNK);
  int LocalY = CellY - (ChunkY * BIOME_CELLS_PER_CHUNK);
  int LocalZ = CellZ - (ChunkZ * BIOME_CELLS_PER_CHUNK);

  return ChunkVal->BiomeMap[LocalX][LocalY][LocalZ];
}

static DdaState InitDDAState(Vec3 RayOrigin, Vec3 RayDir){
  DdaState State = {0};

  float StartX = RayOrigin.x + BLOCK_HALF_SIZE;
  float StartY = RayOrigin.y + BLOCK_HALF_SIZE;
  float StartZ = RayOrigin.z + BLOCK_HALF_SIZE;

  State.VoxelX = (int)__builtin_floorf(StartX);
  State.VoxelY = (int)__builtin_floorf(StartY);
  State.VoxelZ = (int)__builtin_floorf(StartZ);

  State.StepX = (RayDir.x > 0.0F) - (RayDir.x < 0.0F);
  State.StepY = (RayDir.y > 0.0F) - (RayDir.y < 0.0F);
  State.StepZ = (RayDir.z > 0.0F) - (RayDir.z < 0.0F);

  State.TDeltaX = (RayDir.x != 0.0F) ? __builtin_fabsf(BLOCK_SIZE / RayDir.x) : __builtin_huge_valf();
  State.TDeltaY = (RayDir.y != 0.0F) ? __builtin_fabsf(BLOCK_SIZE / RayDir.y) : __builtin_huge_valf();
  State.TDeltaZ = (RayDir.z != 0.0F) ? __builtin_fabsf(BLOCK_SIZE / RayDir.z) : __builtin_huge_valf();

  State.TMaxX = (State.StepX > 0) ? (__builtin_floorf(StartX) + BLOCK_SIZE - StartX) * State.TDeltaX : (StartX - __builtin_floorf(StartX)) * State.TDeltaX;
  State.TMaxY = (State.StepY > 0) ? (__builtin_floorf(StartY) + BLOCK_SIZE - StartY) * State.TDeltaY : (StartY - __builtin_floorf(StartY)) * State.TDeltaY;
  State.TMaxZ = (State.StepZ > 0) ? (__builtin_floorf(StartZ) + BLOCK_SIZE - StartZ) * State.TDeltaZ : (StartZ - __builtin_floorf(StartZ)) * State.TDeltaZ;

  return State;
}

static void StepDDA(DdaState *State){
  if(State->TMaxX < State->TMaxY){
    if(State->TMaxX < State->TMaxZ){
      State->VoxelX += State->StepX;
      State->DistanceTravelled = State->TMaxX;
      State->TMaxX += State->TDeltaX;
      State->LastStep = 0;
    } else {
      State->VoxelZ += State->StepZ;
      State->DistanceTravelled = State->TMaxZ;
      State->TMaxZ += State->TDeltaZ;
      State->LastStep = 2;
    }
  } else {
    if(State->TMaxY < State->TMaxZ){
      State->VoxelY += State->StepY;
      State->DistanceTravelled = State->TMaxY;
      State->TMaxY += State->TDeltaY;
      State->LastStep = 1;
    } else {
      State->VoxelZ += State->StepZ;
      State->DistanceTravelled = State->TMaxZ;
      State->TMaxZ += State->TDeltaZ;
      State->LastStep = 2;
    }
  }
}

RaycastResult RayCastToWorld(World *WorldVal, Vec3 RayOrigin, Vec3 RayDir, float MaxDistance){
  RaycastResult Result = { .Hit = false, .BlockPos ={0.0F, 0.0F, 0.0F}, .BlockId = 0, .Normal = {0.0F, 0.0F, 0.0F} };

  DdaState Dda = InitDDAState(RayOrigin, RayDir);

  int MaxIter = (int)(MaxDistance * DDA_MAX_CROSSED_AXES) + 1; 

  for(int IdxI = 0; IdxI < MaxIter; IdxI++){
    Vec3 CheckPos = { (float)Dda.VoxelX, (float)Dda.VoxelY, (float)Dda.VoxelZ };
    int Id = GetBlockIDFromWorld(WorldVal, CheckPos);

    if(Id != 0){
      Result.Hit = true;
      Result.BlockPos = CheckPos;
      Result.BlockId = Id;

      if(Dda.LastStep == 0) { Result.Normal = (Vec3){ (float)-Dda.StepX, 0.0F, 0.0F }; }
      else if(Dda.LastStep == 1) { Result.Normal = (Vec3){ 0.0F, (float)-Dda.StepY, 0.0F }; }
      else if(Dda.LastStep == 2) { Result.Normal = (Vec3){ 0.0F, 0.0F, (float)-Dda.StepZ }; }

      return Result;
    }

    StepDDA(&Dda);

    if(Dda.DistanceTravelled > MaxDistance) { break; }
  }

  return Result;
}

bool AreNeighborsGenerated(World *WorldVal, Chunk *ChunkVal) {
  int Cx = ChunkVal->ChunkX;
  int Cy = ChunkVal->ChunkY;
  int Cz = ChunkVal->ChunkZ;

#define NEIGHBOR_COUNT 6
  int NeighborCoords[NEIGHBOR_COUNT][3] = {
    {Cx - 1, Cy, Cz},
    {Cx + 1, Cy, Cz},
    {Cx, Cy - 1, Cz},
    {Cx, Cy + 1, Cz},
    {Cx, Cy, Cz - 1},
    {Cx, Cy, Cz + 1}
  };

  for (int IdxI = 0; IdxI < NEIGHBOR_COUNT; IdxI++) {
    Chunk *Neighbor = GetChunkFromWorld(WorldVal, NeighborCoords[IdxI][0], NeighborCoords[IdxI][1], NeighborCoords[IdxI][2]);
    if (Neighbor != (Chunk *)0) {
      if (!Neighbor->IsGenerated || Neighbor->IsGenerating) {
        return false;
      }
    }
  }
#undef NEIGHBOR_COUNT

  return true;
}

void WorldGetLoadLayerRange(const World *WorldVal, Vec3 Center, int RenderDist,
                            LoadLayerRange *Out) {
  LoadVolume Volume;
  BuildLoadVolume(WorldVal, Center, RenderDist, &Volume);

  Out->Lo = Volume.Layers[0];
  Out->Hi = Volume.Layers[0];
  for (int Layer = 1; Layer < Volume.LayerCount; Layer++) {
    if (Volume.Layers[Layer] < Out->Lo) { Out->Lo = Volume.Layers[Layer]; }
    if (Volume.Layers[Layer] > Out->Hi) { Out->Hi = Volume.Layers[Layer]; }
  }
  Out->Count = Volume.LayerCount;
}

bool WorldIsGenerationSettled(World *WorldVal, Vec3 Center, int RenderDist) {
  LoadVolume Volume;
  BuildLoadVolume(WorldVal, Center, RenderDist, &Volume);

  for (int Col = 0; Col < Volume.ColumnCount; Col++) {
    int ChunkX = Volume.CenterX + Volume.Columns[Col].Dx;
    int ChunkZ = Volume.CenterZ + Volume.Columns[Col].Dz;
    for (int Layer = 0; Layer < Volume.LayerCount; Layer++) {
      Chunk *ChunkVal =
          GetChunkFromWorld(WorldVal, ChunkX, Volume.Layers[Layer], ChunkZ);
      if (ChunkVal == (Chunk *)0) {
        return false;
      }
      if (!ChunkVal->IsGenerated || ChunkVal->IsGenerating ||
          ChunkVal->TerrainJustGenerated) {
        return false;
      }
    }
  }
  return true;
}

bool WorldIsSettled(World *WorldVal, Vec3 Center, int RenderDist) {
  if (!WorldIsGenerationSettled(WorldVal, Center, RenderDist)) {
    return false;
  }

  // The same volume UpdateWorld loads, so this asks about exactly the set that
  // was asked for.
  LoadVolume Volume;
  BuildLoadVolume(WorldVal, Center, RenderDist, &Volume);

  for (int Col = 0; Col < Volume.ColumnCount; Col++) {
    int ChunkX = Volume.CenterX + Volume.Columns[Col].Dx;
    int ChunkZ = Volume.CenterZ + Volume.Columns[Col].Dz;
    for (int Layer = 0; Layer < Volume.LayerCount; Layer++) {
      Chunk *ChunkVal =
          GetChunkFromWorld(WorldVal, ChunkX, Volume.Layers[Layer], ChunkZ);
      if (ChunkVal == (Chunk *)0 || ChunkVal->IsDirty || ChunkVal->IsMeshing) {
        return false;
      }
    }
  }
  return true;
}

void WorldGetLoadCounts(World *WorldVal, int *OutGenerated, int *OutGenerating,
                        int *OutDirty, int *OutMeshing) {
  int Generated = 0;
  int Generating = 0;
  int Dirty = 0;
  int Meshing = 0;

  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    if (!WorldVal->SlotInUse[IdxI]) {
      continue;
    }
    Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
    if (ChunkVal->IsGenerated) {
      Generated++;
    }
    if (ChunkVal->IsGenerating) {
      Generating++;
    }
    if (ChunkVal->IsDirty) {
      Dirty++;
    }
    if (ChunkVal->IsMeshing) {
      Meshing++;
    }
  }

  if (OutGenerated != (int *)0) { *OutGenerated = Generated; }
  if (OutGenerating != (int *)0) { *OutGenerating = Generating; }
  if (OutDirty != (int *)0) { *OutDirty = Dirty; }
  if (OutMeshing != (int *)0) { *OutMeshing = Meshing; }
}
