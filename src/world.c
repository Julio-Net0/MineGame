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

  int renderDist = 1;

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

void SetBlockInWorld(World *world, Vector3 globalPos, unsigned char blockID){
  int lx; 
  int ly; 
  int lz;

  Chunk *c = GetLocalCoords(world, globalPos, &lx, &ly, &lz);

  if(c != NULL){
    SetBlockInChunk(c, (Vector3){(float)lx, (float)ly, (float)lz}, blockID);
  }
}

RaycastResult RayCastToWorld(World *world, Vector3 rayOrigin, Vector3 rayDir, float maxDistance){
  RaycastResult result = { .hit = false, .blockPos ={0}, .blockID = 0};

  float step = 0.05F;

  for(float i = 0.0F; i < maxDistance; i+=step){
    Vector3 currentPos = { rayOrigin.x + ( rayDir.x * i ), rayOrigin.y + ( rayDir.y * i ), rayOrigin.z + ( rayDir.z * i ) };

    Vector3 checkPos = {
      currentPos.x + BLOCK_HALF_SIZE,
      currentPos.y + BLOCK_HALF_SIZE,
      currentPos.z + BLOCK_HALF_SIZE
    };

    int id = GetBlockIDFromWorld(world, checkPos);

    if(id != 0){
      result.hit = true;
      result.blockPos = (Vector3){ floorf(checkPos.x), floorf(checkPos.y), floorf(checkPos.z) };
      result.blockID = id;

      Vector3 diff = Vector3Subtract(currentPos, result.blockPos);
      Vector3 absDiff = { fabsf(diff.x), fabsf(diff.y), fabsf(diff.z)};

      if(absDiff.x > absDiff.y && absDiff.x > absDiff.z){
        result.normal = (Vector3){(diff.x > 0 ? 1: -1), 0, 0};
      }else if(absDiff.y > absDiff.x && absDiff.y > absDiff.z){
        result.normal = (Vector3){0, (diff.y > 0 ? 1: -1), 0};
      }else{
        result.normal = (Vector3){0, 0, (diff.z > 0 ? 1: -1)};
      }

      return result;
    }
  }

  return result;
}
