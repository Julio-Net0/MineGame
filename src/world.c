#include "world.h"
#include "chunk.h"
#include "raylib.h"
#include "renderer.h"
#include <math.h>
#include <stddef.h>

#define DDA_MAX_CROSSED_AXES 3.0F

#define HASH_EMPTY -1
#define HASH_DELETED -2

#define SPATIAL_PRIME_X 73856093
#define SPATIAL_PRIME_Y 19349663
#define SPATIAL_PRIME_Z 83492791 
 
typedef struct {
  float tMaxX, tMaxY, tMaxZ;
  float tDeltaX, tDeltaY, tDeltaZ;
  float distanceTravelled;

  int voxelX, voxelY, voxelZ;
  int stepX, stepY, stepZ;
  int lastStep;
} DDAState;

static int HashChunkPos(int x, int y, int z){
  unsigned int h = (unsigned int)((x * SPATIAL_PRIME_X) ^ (y * SPATIAL_PRIME_Y) ^ (z * SPATIAL_PRIME_Z));
  return h % CHUNK_MAP_SIZE;
}

void InitWorld(World *world){
  world->chunkCount = 0;

  for(int i = 0; i < CHUNK_MAP_SIZE; i++){
    world->chunkHashMap[i] = HASH_EMPTY;
  }
}

static void InsertChunkIntoMap(World *world, int chunkIndex) {
  int x = world->chunks[chunkIndex].chunkX;
  int y = world->chunks[chunkIndex].chunkY;
  int z = world->chunks[chunkIndex].chunkZ;
  int hash = HashChunkPos(x, y, z);

  while(world->chunkHashMap[hash] >= 0){
    hash = (hash + 1) % CHUNK_MAP_SIZE;
  }
  world->chunkHashMap[hash] = chunkIndex;
}

static void RemoveChunkFromMap(World *world, int chunkX, int chunkY, int chunkZ) {
  int hash = HashChunkPos(chunkX, chunkY, chunkZ);
  int initialHash = hash;

  while(world->chunkHashMap[hash] != HASH_EMPTY) {
    int idx = world->chunkHashMap[hash];
    if(idx >= 0 && world->chunks[idx].chunkX == chunkX && world->chunks[idx].chunkY == chunkY && world->chunks[idx].chunkZ == chunkZ) {
      world->chunkHashMap[hash] = HASH_DELETED;
      return;
    }
    hash = (hash + 1) % CHUNK_MAP_SIZE;
    if(hash == initialHash) { break; }
  }
}

Chunk* GetChunkFromWorld(World *world, int chunkX, int chunkY, int chunkZ){
  int hash = HashChunkPos(chunkX, chunkY, chunkZ);
  int initialHash = hash;

  while(world->chunkHashMap[hash] != HASH_EMPTY) {
    int idx = world->chunkHashMap[hash];
    if(idx >= 0 && world->chunks[idx].chunkX == chunkX && world->chunks[idx].chunkY == chunkY && world->chunks[idx].chunkZ == chunkZ) {
      return &world->chunks[idx];
    }
    hash = (hash + 1) % CHUNK_MAP_SIZE;
    if(hash == initialHash) { break; }
  }
  return NULL;
}

Chunk* GetChunkAtPos(World *world, Vector3 pos){
  int chunkX = (int)floorf(pos.x / CHUNK_SIZE);
  int chunkY = (int)floorf(pos.y / CHUNK_SIZE);
  int chunkZ = (int)floorf(pos.z / CHUNK_SIZE);

  return GetChunkFromWorld(world, chunkX, chunkY, chunkZ);
}

static void MarkUsefulChunks(World *world, int pChunkX, int pChunkY, int pChunkZ, int renderDist, bool *keepChunk){
  for(int x = pChunkX - renderDist; x <= pChunkX + renderDist; x++){
    for(int y = pChunkY - renderDist; y <= pChunkY + renderDist; y++){
      for(int z = pChunkZ - renderDist; z <= pChunkZ + renderDist; z++){

        Chunk* c = GetChunkFromWorld(world, x, y, z);
        if(c != NULL){
          int idx = (int)(c - world->chunks);
          keepChunk[idx] = true;
        }
      }
    }
  }
}

static void UpdateNeighborsDirtyFlag(World *world, int cx, int cy, int cz){
  Chunk *neighbor;
  neighbor = GetChunkFromWorld(world, cx - 1, cy, cz); if (neighbor) { neighbor->isDirty = true; }
  neighbor = GetChunkFromWorld(world, cx + 1, cy, cz); if (neighbor) { neighbor->isDirty = true; }
  neighbor = GetChunkFromWorld(world, cx, cy - 1, cz); if (neighbor) { neighbor->isDirty = true; }
  neighbor = GetChunkFromWorld(world, cx, cy + 1, cz); if (neighbor) { neighbor->isDirty = true; }
  neighbor = GetChunkFromWorld(world, cx, cy, cz - 1); if (neighbor) { neighbor->isDirty = true; }
  neighbor = GetChunkFromWorld(world, cx, cy, cz + 1); if (neighbor) { neighbor->isDirty = true; }
}

static void CreateOrRecycleChunk(World *world, int chunkX, int chunkY, int chunkZ, bool *keepChunk){
  for(int i = 0; i < MAX_ACTIVE_CHUNKS; i++){
    if(i >= world->chunkCount || !keepChunk[i]){

      if(i < world->chunkCount) { 
        int oldX = world->chunks[i].chunkX;
        int oldY = world->chunks[i].chunkY;
        int oldZ = world->chunks[i].chunkZ;

        RemoveChunkFromMap(world, oldX, oldY, oldZ);

        UpdateNeighborsDirtyFlag(world, oldX, oldY, oldZ);
      }

      if(world->chunks[i].hasMesh){
        UnloadChunkMesh(&world->chunks[i]);
      }

      world->chunks[i].chunkX = chunkX;
      world->chunks[i].chunkY = chunkY;
      world->chunks[i].chunkZ = chunkZ;
      //GenerateFlatChunk(&world->chunks[i]);
      GenerateChunkTerrain(&world->chunks[i]);
      keepChunk[i] = true;
      
      InsertChunkIntoMap(world, i);

      UpdateNeighborsDirtyFlag(world, chunkX, chunkY, chunkZ);

      if(i >= world->chunkCount){
        world->chunkCount++;
      }
      break;
    }
  }
}

void UpdateWorld(World *world, Vector3 playerPos, int renderDist){
  int requiredChunks = (2 * renderDist + 1) * (2 * renderDist + 1);
  if(requiredChunks > MAX_ACTIVE_CHUNKS){
    renderDist = MAX_RENDER_DISTANCE;
  }

  int pChunkX = (int)floorf(playerPos.x / CHUNK_SIZE);
  int pChunkY = (int)floorf(playerPos.y / CHUNK_SIZE);
  int pChunkZ = (int)floorf(playerPos.z / CHUNK_SIZE);

  bool keepChunk[MAX_ACTIVE_CHUNKS] = { false };

  MarkUsefulChunks(world, pChunkX, pChunkY, pChunkZ, renderDist, keepChunk);

  for(int x = pChunkX - renderDist; x <= pChunkX + renderDist; x++){
    for(int y = pChunkY - renderDist; y <= pChunkY + renderDist; y++){
      for(int z = pChunkZ - renderDist; z <= pChunkZ + renderDist; z++){

        if(GetChunkFromWorld(world, x, y, z) == NULL){
          CreateOrRecycleChunk(world, x, y, z, keepChunk);
        }
      }
    }
  }
}

static Chunk* GetLocalCoords(World *world, Vector3 globalPos, int *localX, int *localY, int *localZ){

  int chunkX = (int)floorf(globalPos.x / CHUNK_SIZE);
  int chunkY = (int)floorf(globalPos.y / CHUNK_SIZE);
  int chunkZ = (int)floorf(globalPos.z / CHUNK_SIZE);

  *localX = (int)floorf(globalPos.x) - (chunkX * CHUNK_SIZE);
  *localY = (int)floorf(globalPos.y) - (chunkY * CHUNK_SIZE);
  *localZ = (int)floorf(globalPos.z) - (chunkZ * CHUNK_SIZE);

  return GetChunkFromWorld(world, chunkX, chunkY, chunkZ);
}

void SetBlockInWorld(World *world, Vector3 pos, unsigned char blockID){
  Chunk *chunk = GetChunkAtPos(world, pos);
  if (chunk == NULL) { return; }

  int localX = ((int)floorf(pos.x)) % CHUNK_SIZE;
  int localY = ((int)floorf(pos.y)) % CHUNK_SIZE;
  int localZ = ((int)floorf(pos.z)) % CHUNK_SIZE;

  if (localX < 0) { localX += CHUNK_SIZE; }
  if (localY < 0) { localY += CHUNK_SIZE; }
  if (localZ < 0) { localZ += CHUNK_SIZE; }

  SetBlockInChunk(chunk, (Vector3){(float)localX, (float)localY, (float)localZ}, blockID);
  chunk->isDirty = true;

  if (localX == 0) {
    Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX - 1, chunk->chunkY,chunk->chunkZ);
    if (neighbor) { neighbor->isDirty = true; }
  } 
  else if (localX == CHUNK_SIZE - 1) {
    Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX + 1, chunk->chunkY,chunk->chunkZ);
    if (neighbor) { neighbor->isDirty = true; }
  }

  if (localY == 0) {
    Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX, chunk->chunkY - 1,chunk->chunkZ);
    if (neighbor) { neighbor->isDirty = true; }
  } 
  else if (localY == CHUNK_SIZE - 1) {
    Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX, chunk->chunkY + 1,chunk->chunkZ);
    if (neighbor) { neighbor->isDirty = true; }
  }

  if (localZ == 0) {
    Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX, chunk->chunkY,chunk->chunkZ - 1);
    if (neighbor) { neighbor->isDirty = true; }
  } 
  else if (localZ == CHUNK_SIZE - 1) {
    Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX, chunk->chunkY,chunk->chunkZ + 1);
    if (neighbor) { neighbor->isDirty = true; }
  }
}

int GetBlockIDFromWorld(World *world, Vector3 globalPos){
  int lx; 
  int ly; 
  int lz;

  Chunk *c = GetLocalCoords(world, globalPos, &lx, &ly, &lz);

  if(c != NULL){
    return GetBlockIDInChunk(c, (Vector3){(float)lx, (float)ly, (float)lz});
  }

  return 0;
}

static DDAState InitDDAState(Vector3 rayOrigin, Vector3 rayDir){
  DDAState state = {0};

  float startX = rayOrigin.x + BLOCK_HALF_SIZE;
  float startY = rayOrigin.y + BLOCK_HALF_SIZE;
  float startZ = rayOrigin.z + BLOCK_HALF_SIZE;

  state.voxelX = (int)floorf(startX);
  state.voxelY = (int)floorf(startY);
  state.voxelZ = (int)floorf(startZ);

  state.stepX = (rayDir.x > 0) - (rayDir.x < 0);
  state.stepY = (rayDir.y > 0) - (rayDir.y < 0);
  state.stepZ = (rayDir.z > 0) - (rayDir.z < 0);

  state.tDeltaX = (rayDir.x != 0) ? fabsf(BLOCK_SIZE / rayDir.x) : INFINITY;
  state.tDeltaY = (rayDir.y != 0) ? fabsf(BLOCK_SIZE / rayDir.y) : INFINITY;
  state.tDeltaZ = (rayDir.z != 0) ? fabsf(BLOCK_SIZE / rayDir.z) : INFINITY;

  state.tMaxX = (state.stepX > 0) ? (floorf(startX) + BLOCK_SIZE - startX) * state.tDeltaX : (startX - floorf(startX)) * state.tDeltaX;
  state.tMaxY = (state.stepY > 0) ? (floorf(startY) + BLOCK_SIZE - startY) * state.tDeltaY : (startY - floorf(startY)) * state.tDeltaY;
  state.tMaxZ = (state.stepZ > 0) ? (floorf(startZ) + BLOCK_SIZE - startZ) * state.tDeltaZ : (startZ - floorf(startZ)) * state.tDeltaZ;

  return state;
}

static void StepDDA(DDAState *state){
  if(state->tMaxX < state->tMaxY){
    if(state->tMaxX < state->tMaxZ){
      state->voxelX += state->stepX;
      state->distanceTravelled = state->tMaxX;
      state->tMaxX += state->tDeltaX;
      state->lastStep = 0;
    } else {
      state->voxelZ += state->stepZ;
      state->distanceTravelled = state->tMaxZ;
      state->tMaxZ += state->tDeltaZ;
      state->lastStep = 2;
    }
  } else {
    if(state->tMaxY < state->tMaxZ){
      state->voxelY += state->stepY;
      state->distanceTravelled = state->tMaxY;
      state->tMaxY += state->tDeltaY;
      state->lastStep = 1;
    } else {
      state->voxelZ += state->stepZ;
      state->distanceTravelled = state->tMaxZ;
      state->tMaxZ += state->tDeltaZ;
      state->lastStep = 2;
    }
  }
}

RaycastResult RayCastToWorld(World *world, Vector3 rayOrigin, Vector3 rayDir, float maxDistance){
  RaycastResult result = { .hit = false, .blockPos ={0}, .blockID = 0, .normal = {0} };

  DDAState dda = InitDDAState(rayOrigin, rayDir);

  int maxIter = (int)(maxDistance * DDA_MAX_CROSSED_AXES) + 1; 

  for(int i = 0; i < maxIter; i++){
    Vector3 checkPos = { (float)dda.voxelX, (float)dda.voxelY, (float)dda.voxelZ };
    int id = GetBlockIDFromWorld(world, checkPos);

    if(id != 0){
      result.hit = true;
      result.blockPos = checkPos;
      result.blockID = id;

      if(dda.lastStep == 0) { result.normal = (Vector3){ (float)-dda.stepX, 0, 0 }; }
      else if(dda.lastStep == 1) { result.normal = (Vector3){ 0, (float)-dda.stepY, 0 }; }
      else if(dda.lastStep == 2) { result.normal = (Vector3){ 0, 0, (float)-dda.stepZ }; }

      return result;
    }

    StepDDA(&dda);

    if(dda.distanceTravelled > maxDistance) { break; }
  }

  return result;
}
