#include "chunk.h"
#include "renderer.h"
#include "world.h"
#include "raylib.h"

#define STB_PERLING_IMPLEMENTATION
#include "stb_perling.h"

void GenerateChunkTerrain(Chunk *chunk) {
  int solidCount = 0;

  for (int x = 0; x < CHUNK_SIZE; x++) {
    for (int y = 0; y < CHUNK_SIZE; y++) {
      for (int z = 0; z < CHUNK_SIZE; z++) {
        chunk->data[x][y][z] = 0; 
      }
    }
  }

  for (int x = 0; x < CHUNK_SIZE; x++) {
    for (int z = 0; z < CHUNK_SIZE; z++) {

      int globalX = (chunk->chunkX * CHUNK_SIZE) + x;
      int globalZ = (chunk->chunkZ * CHUNK_SIZE) + z;

      float scale = 0.01f;
      float lacunarity = 2.0f;
      float gain = 0.5f;
      int octaves = 4;

      float noise = stb_perlin_fbm_noise3((float)globalX * scale, 0.0f, (float)globalZ * scale, lacunarity, gain, octaves);

      int terrainHeight = 24 + (int)(noise * 16.0f);

      int seaLevel = 18;

      for (int y = 0; y < CHUNK_SIZE; y++) {
        int globalY = (chunk->chunkY * CHUNK_SIZE) + y;
        unsigned char blockID = 0;

        if (globalY > terrainHeight) {
          if (globalY <= seaLevel) {
            blockID = 5;
          }
        } 
        else if (globalY == terrainHeight) {
          if (globalY <= seaLevel + 1) {
            blockID = 4;
          } else {
            blockID = 1;
          }
        } 
        else if (globalY > terrainHeight - 3) {
          blockID = 2;
        } 
        else {
          blockID = 3;
        }

        chunk->data[x][y][z] = blockID;

        if (blockID != 0) {
          solidCount++;
        }
      }
    }
  }

  chunk->solidBlockCount = solidCount;
  chunk->isDirty = true;
}

void GenerateFlatChunk(Chunk *chunk){

  chunk->solidBlockCount = 0;

  for(int x = 0; x < CHUNK_SIZE; x++){
    for(int y = 0; y < CHUNK_SIZE; y++){
      for(int z = 0; z < CHUNK_SIZE; z++){

        int globalY = (chunk->chunkY * CHUNK_SIZE) + y;

        if(globalY < 4){
          chunk->data[x][y][z] = 3;
          chunk->solidBlockCount++;
        }else if(globalY < 10){
          chunk->data[x][y][z] = 1;
          chunk->solidBlockCount++;
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
    unsigned char oldID = chunk->data[x][y][z];

    if(oldID == 0 && blockID != 0) { chunk->solidBlockCount++; }
    else if(oldID != 0 && blockID == 0) { chunk->solidBlockCount--; }

    chunk->data[x][y][z] = blockID;
    chunk->isDirty = true;
  }
}
