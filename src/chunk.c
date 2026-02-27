#include "chunk.h"
#include "world.h"
#include "block_system.h"
#include "raylib.h"
#include "renderer.h"

void GenerateFlatChunk(Chunk *chunk){
  for(int x = 0; x < CHUNK_SIZE; x++){
    for(int z = 0; z < CHUNK_SIZE; z++){
      for(int y = 0; y < CHUNK_SIZE; y++){
        if(y < 4){
          chunk->data[x][y][z] = 2;
        }else if(y < 10){
          chunk->data[x][y][z] = 1;
        }else{
          chunk->data[x][y][z] = 0;
        }
      }
    }
  }
}

void DrawChunk(World *world, Chunk *chunk){
  for(int x = 0; x < CHUNK_SIZE; x++){
    for(int y = 0; y < CHUNK_SIZE; y++){
      for(int z = 0; z < CHUNK_SIZE; z++){

        unsigned char id = chunk->data[x][y][z];

        if(id == 0){continue;}
        
        BlockType* def = GetBlockDef(id);

        DrawCubeCulled(world, chunk, x, y, z, def->color);

      }
    }
  }
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
  }
}
