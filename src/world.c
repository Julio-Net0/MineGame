#include "world.h"
#include "block_system.h"
#include "renderer.h"

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
