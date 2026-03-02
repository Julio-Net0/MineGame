#include "chunk.h"
#include "renderer.h"
#include "world.h"
#include "raylib.h"

void GenerateFlatChunk(Chunk *chunk){
  for(int x = 0; x < CHUNK_SIZE; x++){
    for(int y = 0; y < CHUNK_SIZE; y++){
      for(int z = 0; z < CHUNK_SIZE; z++){

        int globalY = (chunk->chunkY * CHUNK_SIZE) + y;

        if(globalY < 4){
          chunk->data[x][y][z] = 2;
        }else if(globalY < 10){
          chunk->data[x][y][z] = 1;
        }else{
          chunk->data[x][y][z] = 0;
        }
      }
    }
  }
  chunk->isDirty = true;
  chunk->hasMesh = false;
}

void DrawChunk(World *world, Chunk *chunk){
  
  if(chunk->isDirty){
    BuildChunkMesh(world, chunk);
  }

  RenderChunkMesh(chunk);
}

int GetBlockIDInChunk(Chunk *chunk, Vector3 localPos){

  int x = (int)localPos.x;
  int y = (int)localPos.y;
  int z = (int)localPos.z;

  if(x < 0 || x >= CHUNK_SIZE ||y < 0 || y >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE){ return 0; }

  return (int)chunk->data[x][y][z];
}

void SetBlockInChunk(Chunk *chunk, Vector3 localPos, unsigned char blockID){

  int x = (int)localPos.x;
  int y = (int)localPos.y;
  int z = (int)localPos.z;

  if(x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE){
    chunk->data[x][y][z] = blockID;
    chunk->isDirty = true;
  }
}
