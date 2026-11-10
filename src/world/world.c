#include "world/world.h"
#include "world/chunk.h"
#include "world/chunk_worker.h"
#include "persistence/world_save.h"
#include "core/profiler.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#define DDA_MAX_CROSSED_AXES 3.0F

#define HASH_EMPTY (-1)
#define HASH_DELETED (-2)

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

static int HashChunkPos(int Cx, int Cy, int Cz){
  unsigned int H = (unsigned int)((Cx * SPATIAL_PRIME_X) ^ (Cy * SPATIAL_PRIME_Y) ^ (Cz * SPATIAL_PRIME_Z));
  return (int)(H % (unsigned int)CHUNK_MAP_SIZE);
}

void InitWorld(World *WorldVal){
  WorldVal->ChunkCount = 0;

  for(int IdxI = 0; IdxI < CHUNK_MAP_SIZE; IdxI++){
    WorldVal->ChunkHashMap[IdxI] = HASH_EMPTY;
  }

  WorldVal->FreeCount = MAX_ACTIVE_CHUNKS;
  for(int IdxI = 0; IdxI < MAX_ACTIVE_CHUNKS; IdxI++){
    WorldVal->FreeList[IdxI] = IdxI;
  }

  WorldVal->LastLoadChunkX = 0;
  WorldVal->LastLoadChunkY = 0;
  WorldVal->LastLoadChunkZ = 0;
  WorldVal->LastLoadRenderDist = 0;
  WorldVal->HasLoadedOnce = false;
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

static void InsertChunkIntoMap(World *WorldVal, int ChunkIndex) {
  int Cx = WorldVal->Chunks[ChunkIndex].ChunkX;
  int Cy = WorldVal->Chunks[ChunkIndex].ChunkY;
  int Cz = WorldVal->Chunks[ChunkIndex].ChunkZ;
  int Hash = HashChunkPos(Cx, Cy, Cz);

  while(WorldVal->ChunkHashMap[Hash] >= 0){
    Hash = (Hash + 1) % CHUNK_MAP_SIZE;
  }
  WorldVal->ChunkHashMap[Hash] = ChunkIndex;
}

static void RemoveChunkFromMap(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ) {
  int Hash = HashChunkPos(ChunkX, ChunkY, ChunkZ);
  int InitialHash = Hash;

  while(WorldVal->ChunkHashMap[Hash] != HASH_EMPTY) {
    int Idx = WorldVal->ChunkHashMap[Hash];
    if(Idx >= 0 && WorldVal->Chunks[Idx].ChunkX == ChunkX && WorldVal->Chunks[Idx].ChunkY == ChunkY && WorldVal->Chunks[Idx].ChunkZ == ChunkZ) {
      WorldVal->ChunkHashMap[Hash] = HASH_DELETED;
      return;
    }
    Hash = (Hash + 1) % CHUNK_MAP_SIZE;
    if(Hash == InitialHash) { break; }
  }
}

Chunk* GetChunkFromWorld(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ){
  int Hash = HashChunkPos(ChunkX, ChunkY, ChunkZ);
  int InitialHash = Hash;

  while(WorldVal->ChunkHashMap[Hash] != HASH_EMPTY) {
    int Idx = WorldVal->ChunkHashMap[Hash];
    if(Idx >= 0 && WorldVal->Chunks[Idx].ChunkX == ChunkX && WorldVal->Chunks[Idx].ChunkY == ChunkY && WorldVal->Chunks[Idx].ChunkZ == ChunkZ) {
      return &WorldVal->Chunks[Idx];
    }
    Hash = (Hash + 1) % CHUNK_MAP_SIZE;
    if(Hash == InitialHash) { break; }
  }
  return (Chunk *)0;
}

Chunk* GetChunkAtPos(World *WorldVal, Vec3 Pos){
  int ChunkX = (int)__builtin_floorf(Pos.x / CHUNK_SIZE);
  int ChunkY = (int)__builtin_floorf(Pos.y / CHUNK_SIZE);
  int ChunkZ = (int)__builtin_floorf(Pos.z / CHUNK_SIZE);

  return GetChunkFromWorld(WorldVal, ChunkX, ChunkY, ChunkZ);
}

static void MarkUsefulChunks(World *WorldVal, int PChunkX, int PChunkY, int PChunkZ, int RenderDist, bool *KeepChunk){
  for(int IdxX = PChunkX - RenderDist; IdxX <= PChunkX + RenderDist; IdxX++){
    for(int IdxY = PChunkY - RenderDist; IdxY <= PChunkY + RenderDist; IdxY++){
      for(int IdxZ = PChunkZ - RenderDist; IdxZ <= PChunkZ + RenderDist; IdxZ++){
        Chunk* ChunkVal = GetChunkFromWorld(WorldVal, IdxX, IdxY, IdxZ);
        if(ChunkVal != (Chunk *)0){
          int Idx = (int)(ChunkVal - WorldVal->Chunks);
          KeepChunk[Idx] = true;
        }
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

static void EvictUnneededChunks(World *WorldVal, bool *KeepChunk){
  for(int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++){
    if(KeepChunk[IdxI] || WorldVal->Chunks[IdxI].IsGenerating ||
       WorldVal->Chunks[IdxI].IsMeshing){
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

    KeepChunk[IdxI] = true;
    WorldVal->FreeList[WorldVal->FreeCount++] = IdxI;
  }
}

static bool CreateOrRecycleChunk(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ){
  if(WorldVal->FreeCount == 0){
    return false;
  }

  int IdxI = WorldVal->FreeList[--WorldVal->FreeCount];

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
  int Side = (2 * RenderDist) + 1;
  int RequiredChunks = Side * Side * Side;
  if(RequiredChunks > MAX_ACTIVE_CHUNKS){
    RenderDist = MAX_RENDER_DISTANCE;
  }

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

  bool KeepChunk[MAX_ACTIVE_CHUNKS] = { false };

  MarkUsefulChunks(WorldVal, PChunkX, PChunkY, PChunkZ, RenderDist, KeepChunk);
  EvictUnneededChunks(WorldVal, KeepChunk);

  bool AllCreated = true;
  for(int IdxX = PChunkX - RenderDist; IdxX <= PChunkX + RenderDist; IdxX++){
    for(int IdxY = PChunkY - RenderDist; IdxY <= PChunkY + RenderDist; IdxY++){
      for(int IdxZ = PChunkZ - RenderDist; IdxZ <= PChunkZ + RenderDist; IdxZ++){
        if(GetChunkFromWorld(WorldVal, IdxX, IdxY, IdxZ) == (Chunk *)0){
          if(!CreateOrRecycleChunk(WorldVal, IdxX, IdxY, IdxZ)){
            AllCreated = false;
          }
        }
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

bool WorldIsGenerationSettled(World *WorldVal, Vec3 Center, int RenderDist) {
  int Side = (2 * RenderDist) + 1;
  if ((Side * Side * Side) > MAX_ACTIVE_CHUNKS) {
    RenderDist = MAX_RENDER_DISTANCE;
  }

  int CenterX = (int)__builtin_floorf(Center.x / CHUNK_SIZE);
  int CenterY = (int)__builtin_floorf(Center.y / CHUNK_SIZE);
  int CenterZ = (int)__builtin_floorf(Center.z / CHUNK_SIZE);

  for (int IdxX = CenterX - RenderDist; IdxX <= CenterX + RenderDist; IdxX++) {
    for (int IdxY = CenterY - RenderDist; IdxY <= CenterY + RenderDist; IdxY++) {
      for (int IdxZ = CenterZ - RenderDist; IdxZ <= CenterZ + RenderDist;
           IdxZ++) {
        Chunk *ChunkVal = GetChunkFromWorld(WorldVal, IdxX, IdxY, IdxZ);
        if (ChunkVal == (Chunk *)0) {
          return false;
        }
        if (!ChunkVal->IsGenerated || ChunkVal->IsGenerating ||
            ChunkVal->TerrainJustGenerated) {
          return false;
        }
      }
    }
  }
  return true;
}

bool WorldIsSettled(World *WorldVal, Vec3 Center, int RenderDist) {
  if (!WorldIsGenerationSettled(WorldVal, Center, RenderDist)) {
    return false;
  }

  int Side = (2 * RenderDist) + 1;
  if ((Side * Side * Side) > MAX_ACTIVE_CHUNKS) {
    RenderDist = MAX_RENDER_DISTANCE;
  }

  // The same coordinate derivation UpdateWorld uses to decide what to load, so
  // this asks about exactly the set that was asked for.
  int CenterX = (int)__builtin_floorf(Center.x / CHUNK_SIZE);
  int CenterY = (int)__builtin_floorf(Center.y / CHUNK_SIZE);
  int CenterZ = (int)__builtin_floorf(Center.z / CHUNK_SIZE);

  for (int IdxX = CenterX - RenderDist; IdxX <= CenterX + RenderDist; IdxX++) {
    for (int IdxY = CenterY - RenderDist; IdxY <= CenterY + RenderDist; IdxY++) {
      for (int IdxZ = CenterZ - RenderDist; IdxZ <= CenterZ + RenderDist;
           IdxZ++) {
        Chunk *ChunkVal = GetChunkFromWorld(WorldVal, IdxX, IdxY, IdxZ);
        if (ChunkVal == (Chunk *)0 || ChunkVal->IsDirty || ChunkVal->IsMeshing) {
          return false;
        }
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
