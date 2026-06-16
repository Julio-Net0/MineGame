#include "chunk.h"
#include "raylib.h"
#include "world_save.h"
#include <string.h>

#define STB_PERLING_IMPLEMENTATION
#include "stb_perling.h"

enum BlockIDs {
  BLOCK_AIR = 0,
  BLOCK_GRASS = 1,
  BLOCK_DIRT = 2,
  BLOCK_STONE = 3,
  BLOCK_SAND = 4,
  BLOCK_WATER = 5
};

#define NOISE_SCALE 0.01F
#define NOISE_LACUNARITY 2.0F
#define NOISE_GAIN 0.5F
#define NOISE_OCTAVES 4
#define BASE_HEIGHT 24
#define HEIGHT_VARIANCE 16.0F


static unsigned char DetermineBlockID(int globalY, int terrainHeight) {
  const int SEA_LEVEL = 18;
  if (globalY > terrainHeight) {
    return (globalY <= SEA_LEVEL) ? BLOCK_WATER : BLOCK_AIR;
  }
  if (globalY == terrainHeight) {
    return (globalY <= SEA_LEVEL + 1) ? BLOCK_SAND : BLOCK_GRASS;
  }
  if (globalY > terrainHeight - 3) {
    return BLOCK_DIRT;
  }
  return BLOCK_STONE;
}

void GenerateChunkTerrain(Chunk *chunk) {
  chunk->isModified = false;
  int solidCount = 0;

  uint64_t seed = GetWorldSeed();
  const int SEED_OFFSET_RANGE = 100000;
  const int SEED_OFFSET_HALF = 50000;
  const int SEED_SHIFT_UPPER = 32;
  float offsetX = (float)((int)(seed % SEED_OFFSET_RANGE) - SEED_OFFSET_HALF);
  float offsetZ =
      (float)((int)((seed >> SEED_SHIFT_UPPER) % SEED_OFFSET_RANGE) -
              SEED_OFFSET_HALF);

  memset(chunk->data, 0, sizeof(chunk->data));

  for (int x = 0; x < CHUNK_SIZE; x++) {
    for (int z = 0; z < CHUNK_SIZE; z++) {
      int globalX = (chunk->chunkX * CHUNK_SIZE) + x;
      int globalZ = (chunk->chunkZ * CHUNK_SIZE) + z;

      float noise = stb_perlin_fbm_noise3(
          ((float)globalX + offsetX) * NOISE_SCALE, 0.0F,
          ((float)globalZ + offsetZ) * NOISE_SCALE, NOISE_LACUNARITY,
          NOISE_GAIN, NOISE_OCTAVES);

      int terrainHeight = BASE_HEIGHT + (int)(noise * HEIGHT_VARIANCE);

      for (int y = 0; y < CHUNK_SIZE; y++) {
        int globalY = (chunk->chunkY * CHUNK_SIZE) + y;
        unsigned char blockID = DetermineBlockID(globalY, terrainHeight);

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
