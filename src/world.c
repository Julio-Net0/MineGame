#include "world.h"
#include "block_system.h"
#include "raylib.h"
#include "renderer.h"
#include <math.h>

void GenerateFlatChunk(Chunk *chunk){
  for(int x = 0; x < CHUNK_SIZE; x++){
    for(int z = 0; z < CHUNK_SIZE; z++){
      for(int y = 0; y < CHUNK_SIZE; y++){
        if(y < 13){
          chunk->data[x][y][z] = 2;
        }else if(y < 16){
          chunk->data[x][y][z] = 1;
        }else{
          chunk->data[x][y][z] = 0;
        }
      }
    }
  }
}

void DrawChunk(Chunk *chunk){
  for(int x = 0; x < CHUNK_SIZE; x++){
    for(int y = 0; y < CHUNK_SIZE; y++){
      for(int z = 0; z < CHUNK_SIZE; z++){

        unsigned char id = chunk->data[x][y][z];

        if(id == 0){continue;}
        
        BlockType* def = GetBlockDef(id);

        DrawCubeCulled(chunk, x, y, z, def->color);

      }
    }
  }
}

RaycastResult RayCastToBlock(Chunk *chunk, Vector3 rayOrigin, Vector3 rayDir, float maxDistance){
  RaycastResult result = { .hit = false, .blockPos ={0}, .blockID = 0};

  float step = 0.05F;

  for(float i = 0.0F; i < maxDistance; i+=step){
    Vector3 currentPos = {
      rayOrigin.x + rayDir.x * i,
      rayOrigin.y + rayDir.y * i,
      rayOrigin.z + rayDir.z * i
    };
    
    int x = (int)roundf(currentPos.x);
    int y = (int)roundf(currentPos.y);
    int z = (int)roundf(currentPos.z);

    if(x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE){
      unsigned char id = chunk->data[x][y][z];

      if(id != 0){
        result.hit = true;
        result.blockPos = (Vector3){ (float)x, (float)y, (float)z };
        result.blockID = id;
        return result;
      }
    }
  }

  return result;
}
