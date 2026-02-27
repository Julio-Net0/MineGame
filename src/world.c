#include "world.h"
#include "chunk.h"
#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include <stddef.h>

void InitWorld(World *world){
  world->chunkCount = 0;
}

static int GetChunkIndex(World *world, int chunkX, int chunkZ){
  for(int i = 0; i < world->chunkCount; i++){
    if(world->chunks[i].chunkX == chunkX && world->chunks[i].chunkZ == chunkZ){
      return i;
    }
  }
  return -1;
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

void UpdateWorld(World *world, Vector3 playerPos){
  int pChunkX = (int)floorf(playerPos.x / CHUNK_SIZE);
  int pChunkZ = (int)floorf(playerPos.z / CHUNK_SIZE);

  int renderDist = 10;

  bool keepChunk[MAX_ACTIVE_CHUNKS] = {false};

  MarkUsefulChunks(world, pChunkX, pChunkZ, renderDist, keepChunk);

  for(int x = pChunkX - renderDist; x <= pChunkX + renderDist; x++){
    for(int z = pChunkZ - renderDist; z <= pChunkZ + renderDist; z++){

      if(GetChunkIndex(world, x, z) == -1){
        CreateOrRecycleChunk(world, x, z, keepChunk);
      }
    }
  }
}

void DrawWorld(World *world){
  for(int i = 0; i < world->chunkCount; i++){
    DrawChunk(world, &world->chunks[i]);
  }
}

Chunk* GetLocalCoords(World *world, Vector3 globalPos, int *localX, int *localY, int *localZ){
  *localY = (int)floorf(globalPos.y);

  if(*localY < 0 || *localY >= CHUNK_SIZE) { return NULL; }

  int chunkX = (int)floorf(globalPos.x / CHUNK_SIZE);
  int chunkZ = (int)floorf(globalPos.z / CHUNK_SIZE);

  *localX = (int)floorf(globalPos.x) - (chunkX * CHUNK_SIZE);
  *localZ = (int)floorf(globalPos.z) - (chunkZ * CHUNK_SIZE);

  for(int i = 0; i < world->chunkCount; i++){
    if(world->chunks[i].chunkX == chunkX && world->chunks[i].chunkZ == chunkZ){
      return &world->chunks[i];
    }
  }
  return NULL;
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

RaycastResult RayCastToWorld(World *world, Vector3 rayOrigin, Vector3 rayDir, float maxDistance){
  RaycastResult result = { .hit = false, .blockPos ={0}, .blockID = 0, .normal = {0} };

  float startX = rayOrigin.x + 0.5f;
  float startY = rayOrigin.y + 0.5f;
  float startZ = rayOrigin.z + 0.5f;

  int voxelX = (int)floorf(startX);
  int voxelY = (int)floorf(startY);
  int voxelZ = (int)floorf(startZ);

  int stepX = (rayDir.x > 0) ? 1 : ((rayDir.x < 0) ? -1 : 0);
  int stepY = (rayDir.y > 0) ? 1 : ((rayDir.y < 0) ? -1 : 0);
  int stepZ = (rayDir.z > 0) ? 1 : ((rayDir.z < 0) ? -1 : 0);

  float tDeltaX = (rayDir.x != 0) ? fabsf(1.0f / rayDir.x) : 999999.0f;
  float tDeltaY = (rayDir.y != 0) ? fabsf(1.0f / rayDir.y) : 999999.0f;
  float tDeltaZ = (rayDir.z != 0) ? fabsf(1.0f / rayDir.z) : 999999.0f;

  float tMaxX = (stepX > 0) ? (floorf(startX) + 1.0f - startX) * tDeltaX : (startX - floorf(startX)) * tDeltaX;
  float tMaxY = (stepY > 0) ? (floorf(startY) + 1.0f - startY) * tDeltaY : (startY - floorf(startY)) * tDeltaY;
  float tMaxZ = (stepZ > 0) ? (floorf(startZ) + 1.0f - startZ) * tDeltaZ : (startZ - floorf(startZ)) * tDeltaZ;

  int lastStep = 0;
  float distanceTravelled = 0.0f;

  int maxIter = (int)(maxDistance * 3.0f) + 1; 

  for(int i = 0; i < maxIter; i++){

    Vector3 checkPos = { (float)voxelX, (float)voxelY, (float)voxelZ };
    int id = GetBlockIDFromWorld(world, checkPos);

    if(id != 0){
      result.hit = true;
      result.blockPos = checkPos;
      result.blockID = id;

      if(lastStep == 0) result.normal = (Vector3){ (float)-stepX, 0, 0 };
      else if(lastStep == 1) result.normal = (Vector3){ 0, (float)-stepY, 0 };
      else if(lastStep == 2) result.normal = (Vector3){ 0, 0, (float)-stepZ };

      return result;
    }

    if(tMaxX < tMaxY){
      if(tMaxX < tMaxZ){
        voxelX += stepX;
        distanceTravelled = tMaxX;
        tMaxX += tDeltaX;
        lastStep = 0;
      } else {
        voxelZ += stepZ;
        distanceTravelled = tMaxZ;
        tMaxZ += tDeltaZ;
        lastStep = 2;
      }
    } else {
      if(tMaxY < tMaxZ){
        voxelY += stepY;
        distanceTravelled = tMaxY;
        tMaxY += tDeltaY;
        lastStep = 1;
      } else {
        voxelZ += stepZ;
        distanceTravelled = tMaxZ;
        tMaxZ += tDeltaZ;
        lastStep = 2;
      }
    }

    if(distanceTravelled > maxDistance) break;
  }

  return result;
}
