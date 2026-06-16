#include "chunk.h"
#include "raylib.h"
#include "world_save.h"
#include <string.h>

#define STB_PERLING_IMPLEMENTATION
#include "stb_perling.h"

static unsigned char DetermineBlockID(int globalY, int terrainHeight, int seaLevel) {
  if (globalY > terrainHeight) {
    return (globalY <= seaLevel) ? 5 : 0;
  }
  if (globalY == terrainHeight) {
    return (globalY <= seaLevel + 1) ? 4 : 1;
  }
  if (globalY > terrainHeight - 3) {
    return 2;
  }
  return 3;
}

void GenerateChunkTerrain(Chunk *chunk) {
  chunk->isModified = false;
  int solidCount = 0;

  uint64_t seed = GetWorldSeed();
  float offsetX = (float)((int)(seed % 100000) - 50000);
  float offsetZ = (float)((int)((seed >> 32) % 100000) - 50000);

  memset(chunk->data, 0, sizeof(chunk->data));

  for (int x = 0; x < CHUNK_SIZE; x++) {
    for (int z = 0; z < CHUNK_SIZE; z++) {
      int globalX = (chunk->chunkX * CHUNK_SIZE) + x;
      int globalZ = (chunk->chunkZ * CHUNK_SIZE) + z;

      float scale = 0.01f;
      float lacunarity = 2.0f;
      float gain = 0.5f;
      int octaves = 4;

      float noise = stb_perlin_fbm_noise3(
          ((float)globalX + offsetX) * scale, 0.0f,
          ((float)globalZ + offsetZ) * scale, lacunarity, gain, octaves);

      int terrainHeight = 24 + (int)(noise * 16.0f);

      int seaLevel = 18;

      for (int y = 0; y < CHUNK_SIZE; y++) {
        int globalY = (chunk->chunkY * CHUNK_SIZE) + y;
        unsigned char blockID = DetermineBlockID(globalY, terrainHeight, seaLevel);

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

int GetBlockIDInChunk(Chunk *chunk, Vector3 localPos) {

  int x = (int)localPos.x;
  int y = (int)localPos.y;
  int z = (int)localPos.z;

  if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_SIZE || z < 0 ||
      z >= CHUNK_SIZE) {
    return 0;
  }

  return (int)chunk->data[x][y][z];
}

void SetBlockInChunk(Chunk *chunk, Vector3 localPos, unsigned char blockID) {

  int x = (int)localPos.x;
  int y = (int)localPos.y;
  int z = (int)localPos.z;

  if (x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 &&
      z < CHUNK_SIZE) {
    unsigned char oldID = chunk->data[x][y][z];

    if (oldID != blockID) {
      if (oldID == 0 && blockID != 0) {
        chunk->solidBlockCount++;
      } else if (oldID != 0 && blockID == 0) {
        chunk->solidBlockCount--;
      }

      chunk->data[x][y][z] = blockID;
      chunk->isDirty = true;
      chunk->isModified = true;
    }
  }
}
