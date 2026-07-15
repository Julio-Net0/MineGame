#include "world/chunk.h"
#include "third_party/stb_perlin.h"
#include "persistence/world_save.h"
#include <stdint.h>

enum BlockIDs {
  BLOCK_AIR = 0,
  BLOCK_GRASS = 1,
  BLOCK_DIRT = 2,
  BLOCK_STONE = 3,
  BLOCK_SAND = 4,
  BLOCK_WATER = 5
};

enum {
  NOISE_OCTAVES = 4,
  BASE_HEIGHT = 24
};

#define NOISE_SCALE 0.01F
#define NOISE_LACUNARITY 2.0F
#define NOISE_GAIN 0.5F
#define HEIGHT_VARIANCE 16.0F

enum {
  SEA_LEVEL = 18
};

static void GetTerrainNoiseOffsets(uint64_t SeedVal, float *OffsetX,
                                   float *OffsetZ) {
  const int SEED_OFFSET_RANGE = 100000;
  const int SEED_OFFSET_HALF = 50000;
  const unsigned int SEED_SHIFT_UPPER = 32U;

  *OffsetX =
      (float)((int)(SeedVal % (uint64_t)SEED_OFFSET_RANGE) - SEED_OFFSET_HALF);
  *OffsetZ = (float)((int)((SeedVal >> SEED_SHIFT_UPPER) %
                           (uint64_t)SEED_OFFSET_RANGE) -
                     SEED_OFFSET_HALF);
}

// Hot path: takes the offsets already hoisted out of the caller's loop, so
// generating a chunk still derives them once rather than once per column.
static int TerrainHeightFromOffsets(int GlobalX, int GlobalZ, float OffsetX,
                                    float OffsetZ) {
  float NoiseVal = stb_perlin_fbm_noise3(
      ((float)GlobalX + OffsetX) * NOISE_SCALE, 0.0F,
      ((float)GlobalZ + OffsetZ) * NOISE_SCALE, NOISE_LACUNARITY, NOISE_GAIN,
      NOISE_OCTAVES);
  return BASE_HEIGHT + (int)(NoiseVal * HEIGHT_VARIANCE);
}

int GetTerrainHeightAt(int GlobalX, int GlobalZ, uint64_t SeedVal) {
  float OffsetX = 0.0F;
  float OffsetZ = 0.0F;
  GetTerrainNoiseOffsets(SeedVal, &OffsetX, &OffsetZ);
  return TerrainHeightFromOffsets(GlobalX, GlobalZ, OffsetX, OffsetZ);
}

static unsigned char DetermineBlockId(int GlobalY, int TerrainHeight) {
  if (GlobalY > TerrainHeight) {
    return (GlobalY <= SEA_LEVEL) ? BLOCK_WATER : BLOCK_AIR;
  }
  if (GlobalY == TerrainHeight) {
    return (GlobalY <= SEA_LEVEL + 1) ? BLOCK_SAND : BLOCK_GRASS;
  }
  if (GlobalY > TerrainHeight - 3) {
    return BLOCK_DIRT;
  }
  return BLOCK_STONE;
}

void GenerateChunkTerrain(Chunk *ChunkVal) {
  ChunkVal->IsModified = false;
  int SolidCount = 0;

  uint64_t SeedVal = GetWorldSeed();
  float OffsetX = 0.0F;
  float OffsetZ = 0.0F;
  GetTerrainNoiseOffsets(SeedVal, &OffsetX, &OffsetZ);

  #pragma unroll 4
  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    #pragma unroll 4
    for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
      #pragma unroll 4
      for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
        ChunkVal->Data[IdxX][IdxY][IdxZ] = 0;
      }
    }
  }

  #pragma unroll 4
  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    #pragma unroll 4
    for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
      int GlobalX = (ChunkVal->ChunkX * CHUNK_SIZE) + IdxX;
      int GlobalZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + IdxZ;

      int TerrainHeight =
          TerrainHeightFromOffsets(GlobalX, GlobalZ, OffsetX, OffsetZ);

      #pragma unroll 4
      for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
        int GlobalY = (ChunkVal->ChunkY * CHUNK_SIZE) + IdxY;
        unsigned char BlockId = DetermineBlockId(GlobalY, TerrainHeight);

        ChunkVal->Data[IdxX][IdxY][IdxZ] = BlockId;

        if (BlockId != 0) {
          SolidCount++;
        }
      }
    }
  }

  ChunkVal->SolidBlockCount = SolidCount;
  ChunkVal->IsDirty = true;
}

int GetBlockIdInChunk(Chunk *ChunkVal, Vec3 LocalPos) {
  int IdxX = (int)LocalPos.x;
  int IdxY = (int)LocalPos.y;
  int IdxZ = (int)LocalPos.z;

  if (IdxX < 0 || IdxX >= CHUNK_SIZE || IdxY < 0 || IdxY >= CHUNK_SIZE || IdxZ < 0 ||
      IdxZ >= CHUNK_SIZE) {
    return 0;
  }

  return (int)ChunkVal->Data[IdxX][IdxY][IdxZ];
}

void SetBlockInChunk(Chunk *ChunkVal, Vec3 LocalPos, unsigned char BlockId) {
  int IdxX = (int)LocalPos.x;
  int IdxY = (int)LocalPos.y;
  int IdxZ = (int)LocalPos.z;

  if (IdxX >= 0 && IdxX < CHUNK_SIZE && IdxY >= 0 && IdxY < CHUNK_SIZE && IdxZ >= 0 &&
      IdxZ < CHUNK_SIZE) {
    unsigned char OldId = ChunkVal->Data[IdxX][IdxY][IdxZ];

    if (OldId != BlockId) {
      if (OldId == 0 && BlockId != 0) {
        ChunkVal->SolidBlockCount++;
      } else if (OldId != 0 && BlockId == 0) {
        ChunkVal->SolidBlockCount--;
      }

      ChunkVal->Data[IdxX][IdxY][IdxZ] = BlockId;
      ChunkVal->IsDirty = true;
      ChunkVal->IsModified = true;
    }
  }
}
