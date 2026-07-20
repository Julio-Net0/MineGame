#include "world/chunk.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "core/log.h"
#include "core/noise.h"
#include "persistence/world_save.h"
#include <stdint.h>

// Air is structurally 0 throughout the engine: an empty Data cell means air, and
// SolidBlockCount counts non-zero entries. The blocks that vary by region come
// from the biome palette instead; only the two that no biome owns are resolved
// here, by name, so nothing in generation is a compiled-in block id.
enum {
  BLOCK_AIR = 0
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

// Base blocks no biome owns, resolved once from the registry by name. Written
// only by InitTerrainGeneration, before the workers start, and read-only after.
typedef struct {
  unsigned char StoneId;
  unsigned char WaterId;
} TerrainBaseBlocks;

static TerrainBaseBlocks *GetTerrainBaseBlocks(void) {
  static TerrainBaseBlocks SBase = {0, 0};
  return &SBase;
}

static unsigned char ResolveBaseBlock(const char *Name) {
  int BlockId = GetBlockIdByName(Name);
  if (BlockId < 0) {
    LogError("CHUNK: base block '%s' is not loaded; terrain will use air", Name);
    return BLOCK_AIR;
  }
  return (unsigned char)BlockId;
}

void InitTerrainGeneration(void) {
  TerrainBaseBlocks *Base = GetTerrainBaseBlocks();
  Base->StoneId = ResolveBaseBlock("Stone");
  Base->WaterId = ResolveBaseBlock("Water");
}

static int ClampCell(int Cell) {
  if (Cell < 0) {
    return 0;
  }
  if (Cell >= BIOME_CELLS_PER_CHUNK) {
    return BIOME_CELLS_PER_CHUNK - 1;
  }
  return Cell;
}

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
  float NoiseVal = NoiseFbm3(((float)GlobalX + OffsetX) * NOISE_SCALE, 0.0F,
                             ((float)GlobalZ + OffsetZ) * NOISE_SCALE,
                             NOISE_LACUNARITY, NOISE_GAIN, NOISE_OCTAVES);
  return BASE_HEIGHT + (int)(NoiseVal * HEIGHT_VARIANCE);
}

int GetTerrainHeightAt(int GlobalX, int GlobalZ, uint64_t SeedVal) {
  float OffsetX = 0.0F;
  float OffsetZ = 0.0F;
  GetTerrainNoiseOffsets(SeedVal, &OffsetX, &OffsetZ);
  return TerrainHeightFromOffsets(GlobalX, GlobalZ, OffsetX, OffsetZ);
}

// Air above the terrain, water up to sea level, and stone below the subsurface
// layer are the biome-independent base. Everything between — the surface block
// and the subsurface layer under it — comes from the biome's palette.
static unsigned char DetermineBlockId(const BiomeType *Biome, int GlobalY,
                                      int TerrainHeight) {
  const TerrainBaseBlocks *Base = GetTerrainBaseBlocks();

  if (GlobalY > TerrainHeight) {
    return (GlobalY <= SEA_LEVEL) ? Base->WaterId : BLOCK_AIR;
  }
  if (GlobalY == TerrainHeight) {
    return (GlobalY <= SEA_LEVEL + 1) ? (unsigned char)Biome->UnderwaterBlockId
                                      : (unsigned char)Biome->SurfaceBlockId;
  }
  if (GlobalY > TerrainHeight - Biome->SubsurfaceDepth) {
    return (unsigned char)Biome->SubsurfaceBlockId;
  }
  return Base->StoneId;
}

unsigned char GetChunkBiomeAtLocal(const Chunk *ChunkVal, int LocalX, int LocalY,
                                   int LocalZ) {
  int CellX = LocalX / BIOME_CELL_SIZE;
  int CellY = LocalY / BIOME_CELL_SIZE;
  int CellZ = LocalZ / BIOME_CELL_SIZE;

  CellX = ClampCell(CellX);
  CellY = ClampCell(CellY);
  CellZ = ClampCell(CellZ);

  return ChunkVal->BiomeMap[CellX][CellY][CellZ];
}

void FillChunkBiomeMap(Chunk *ChunkVal) {
  uint64_t SeedVal = GetWorldSeed();
  const int CELL_CENTER = BIOME_CELL_SIZE / 2;

  #pragma unroll 4
  for (int CellX = 0; CellX < BIOME_CELLS_PER_CHUNK; CellX++) {
    #pragma unroll 4
    for (int CellZ = 0; CellZ < BIOME_CELLS_PER_CHUNK; CellZ++) {
      int GlobalX = (ChunkVal->ChunkX * CHUNK_SIZE) + (CellX * BIOME_CELL_SIZE) +
                    CELL_CENTER;
      int GlobalZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + (CellZ * BIOME_CELL_SIZE) +
                    CELL_CENTER;
      int TerrainHeight = GetTerrainHeightAt(GlobalX, GlobalZ, SeedVal);

      // Temperature and humidity depend only on (X, Z), so the column is sampled
      // once and only the derived Depth varies down it: 16 climate samples per
      // chunk rather than 64, for the same 64 resolutions.
      BiomeClimate Climate = SampleBiomeClimate(GlobalX, TerrainHeight, GlobalZ,
                                                TerrainHeight, SeedVal);

      #pragma unroll 4
      for (int CellY = 0; CellY < BIOME_CELLS_PER_CHUNK; CellY++) {
        int GlobalY = (ChunkVal->ChunkY * CHUNK_SIZE) +
                      (CellY * BIOME_CELL_SIZE) + CELL_CENTER;
        Climate.Depth = (float)(TerrainHeight - GlobalY);
        ChunkVal->BiomeMap[CellX][CellY][CellZ] =
            (unsigned char)ResolveBiome(Climate);
      }
    }
  }
}

// Requires BiomeMap to be filled: the caller runs FillChunkBiomeMap first, so
// that generated and disk-loaded chunks share one fill site.
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
        const BiomeType *Biome =
            GetBiomeDef(GetChunkBiomeAtLocal(ChunkVal, IdxX, IdxY, IdxZ));
        unsigned char BlockId = DetermineBlockId(Biome, GlobalY, TerrainHeight);

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
