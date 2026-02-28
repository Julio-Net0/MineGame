#include "world.h"
#include "chunk.h"
#include "raylib.h"
#include "renderer.h"
#include <math.h>
#include <stddef.h>

#define DDA_MAX_CROSSED_AXES 3.0F

typedef struct {
  float tMaxX, tMaxY, tMaxZ;
  float tDeltaX, tDeltaY, tDeltaZ;
  float distanceTravelled;

  int voxelX, voxelY, voxelZ;
  int stepX, stepY, stepZ;
  int lastStep;
} DDAState;

void InitWorld(World *world){
  world->chunkCount = 0;
}

Chunk* GetChunkFromWorld(World *world, int chunkX, int chunkZ){
  for(int i = 0; i < world->chunkCount; i++){
    if(world->chunks[i].chunkX == chunkX && world->chunks[i].chunkZ == chunkZ){
      return &world->chunks[i];
    }
  }
  return NULL;
}

Chunk* GetChunkAtPos(World *world, Vector3 pos){
  int chunkX = (int)floorf(pos.x / CHUNK_SIZE);
  int chunkZ = (int)floorf(pos.z / CHUNK_SIZE);

  return GetChunkFromWorld(world, chunkX, chunkZ);
}

static void MarkUsefulChunks(World *world, int pChunkX, int pChunkZ, int renderDist, bool *keepChunk){
  for(int x = pChunkX - renderDist; x <= pChunkX + renderDist; x++){
    for(int z = pChunkZ - renderDist; z <= pChunkZ + renderDist; z++){
      for(int i = 0; i < world->chunkCount; i++){
        if(world->chunks[i].chunkX == x && world->chunks[i].chunkZ == z){
          keepChunk[i] = true; 
          break;
        }
      }
    }
  }
}

static void CreateOrRecycleChunk(World *world, int chunkX, int chunkZ, bool *keepChunk){
  for(int i = 0; i < MAX_ACTIVE_CHUNKS; i++){
    if(i >= world->chunkCount || !keepChunk[i]){

      if(world->chunks[i].hasMesh){
        UnloadChunkMesh(&world->chunks[i]);
      }

      world->chunks[i].chunkX = chunkX;
      world->chunks[i].chunkZ = chunkZ;
      GenerateFlatChunk(&world->chunks[i]);
      keepChunk[i] = true;
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
  int pChunkZ = (int)floorf(playerPos.z / CHUNK_SIZE);

  bool keepChunk[MAX_ACTIVE_CHUNKS] = { false };

  MarkUsefulChunks(world, pChunkX, pChunkZ, renderDist, keepChunk);

  for(int x = pChunkX - renderDist; x <= pChunkX + renderDist; x++){
    for(int z = pChunkZ - renderDist; z <= pChunkZ + renderDist; z++){

      if(GetChunkFromWorld(world, x, z) == NULL){
        CreateOrRecycleChunk(world, x, z, keepChunk);
      }
    }
  }
}

static Chunk* GetLocalCoords(World *world, Vector3 globalPos, int *localX, int *localY, int *localZ){
  *localY = (int)floorf(globalPos.y);

  if(*localY < 0 || *localY >= CHUNK_SIZE) { return NULL; }

  int chunkX = (int)floorf(globalPos.x / CHUNK_SIZE);
  int chunkZ = (int)floorf(globalPos.z / CHUNK_SIZE);

  *localX = (int)floorf(globalPos.x) - (chunkX * CHUNK_SIZE);
  *localZ = (int)floorf(globalPos.z) - (chunkZ * CHUNK_SIZE);

  return GetChunkFromWorld(world, chunkX, chunkZ);
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
        Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX - 1, chunk->chunkZ);
        if (neighbor) { neighbor->isDirty = true; }
    } 
    else if (localX == CHUNK_SIZE - 1) {
        Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX + 1, chunk->chunkZ);
        if (neighbor) { neighbor->isDirty = true; }
    }

    if (localZ == 0) {
        Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX, chunk->chunkZ - 1);
        if (neighbor) { neighbor->isDirty = true; }
    } 
    else if (localZ == CHUNK_SIZE - 1) {
        Chunk *neighbor = GetChunkFromWorld(world, chunk->chunkX, chunk->chunkZ + 1);
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
