#include "world.h"
#include "block_system.h"
#include "raylib.h"

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
    for(int z = 0; z < CHUNK_SIZE; z++){
      for(int y = 0; y < CHUNK_SIZE; y++){
        unsigned char id = chunk->data[x][y][z];

        if(id == 0){continue;}
        
        BlockType* def = GetBlockDef(id);

        Vector3 blockPos = {
          chunk->position.x + x,
          chunk->position.y + y,
          chunk->position.z + z
        };

        DrawCube(blockPos, 1.0F, 1.0F, 1.0F, def->color);
        DrawCubeWires(blockPos, 1.0F, 1.0F, 1.0F, BLACK);
      }
    }
  }
}
