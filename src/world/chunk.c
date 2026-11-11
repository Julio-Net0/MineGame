#include "world/chunk.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "core/log.h"
#include "persistence/world_save.h"
#include "core/hash.h"
#include "world/surface.h"
#include "world/worldgen_column_cache.h"
#include "world/worldgen_strata.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Air is structurally 0 throughout the engine: an empty Data cell means air, and
// SolidBlockCount counts non-zero entries. The blocks that vary by region come
// from the biome palette instead; only the few that no biome owns are resolved
// here, by name, so nothing in generation is a compiled-in block id.
enum {
  BLOCK_AIR = 0
};

// SEA_LEVEL is declared in chunk.h so the feature pass shares one threshold.

// Base blocks no biome owns, resolved once from the registry by name. Written
// only by InitTerrainGeneration, before the workers start, and read-only after.
typedef struct {
  BlockID StoneId;
  BlockID WaterId;
  BlockID BedrockId;
} TerrainBaseBlocks;

#define BEDROCK_HASH_SALT 0xBEDB0C4000000001ULL

static TerrainBaseBlocks *GetTerrainBaseBlocks(void) {
  static TerrainBaseBlocks SBase = {0, 0, 0};
  return &SBase;
}

static BlockID ResolveBaseBlock(const char *Name) {
  int BlockId = GetBlockIdByName(Name);
  if (BlockId < 0) {
    LogError("CHUNK: base block '%s' is not loaded; terrain will use air", Name);
    return BLOCK_AIR;
  }
  return (BlockID)BlockId;
}

void InitTerrainGeneration(void) {
  TerrainBaseBlocks *Base = GetTerrainBaseBlocks();
  Base->StoneId = ResolveBaseBlock("Stone");
  Base->WaterId = ResolveBaseBlock("Water");
  Base->BedrockId = ResolveBaseBlock("bedrock");
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

// Air above the terrain, water up to sea level, and stone below the subsurface
// layer are the biome-independent base. Everything between — the surface block
// and the subsurface layer under it — comes from the biome's palette.
static BlockID DetermineBlockId(const BiomeType *Biome, int GlobalY,
                                int TerrainHeight) {
  const TerrainBaseBlocks *Base = GetTerrainBaseBlocks();

  if (GlobalY > TerrainHeight) {
    return (GlobalY <= SEA_LEVEL) ? Base->WaterId : BLOCK_AIR;
  }
  if (GlobalY == TerrainHeight) {
    return (GlobalY <= SEA_LEVEL + 1) ? (BlockID)Biome->UnderwaterBlockId
                                      : (BlockID)Biome->SurfaceBlockId;
  }
  if (GlobalY > TerrainHeight - Biome->SubsurfaceDepth) {
    return (BlockID)Biome->SubsurfaceBlockId;
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

void FillChunkBiomeMap(Chunk *ChunkVal, const ColumnEntry *Column) {
  const int CELL_CENTER = BIOME_CELL_SIZE / 2;

  for (int CellX = 0; CellX < BIOME_CELLS_PER_CHUNK; CellX++) {
    for (int CellZ = 0; CellZ < BIOME_CELLS_PER_CHUNK; CellZ++) {
      int Surface = Column->Surface[(CellX * BIOME_CELL_SIZE) + CELL_CENTER]
                                   [(CellZ * BIOME_CELL_SIZE) + CELL_CENTER];

      // Temperature and humidity depend only on (X, Z) and come from the column
      // entry; only the derived Depth varies down the column.
      BiomeClimate Climate = {.Temperature = Column->Temperature[CellX][CellZ],
                              .Humidity = Column->Humidity[CellX][CellZ],
                              .Depth = 0.0F};

      for (int CellY = 0; CellY < BIOME_CELLS_PER_CHUNK; CellY++) {
        int GlobalY = (ChunkVal->ChunkY * CHUNK_SIZE) +
                      (CellY * BIOME_CELL_SIZE) + CELL_CENTER;
        Climate.Depth = (float)(Surface - GlobalY);
        ChunkVal->BiomeMap[CellX][CellY][CellZ] =
            (unsigned char)ResolveBiome(Climate);
      }
    }
  }
}

// Bedrock band: the bottom layer is a closed floor, and each layer above it is
// bedrock with linearly falling probability (5 layers: 1.0, 0.8, 0.6, 0.4, 0.2),
// which breaks up the top of the band the way a natural floor looks.
static BlockID DetermineBedrockBandBlock(int GlobalX, int GlobalY, int GlobalZ,
                                         uint64_t SeedVal) {
  const TerrainBaseBlocks *Base = GetTerrainBaseBlocks();
  int BandMinY = 0;
  int BandMaxY = 0;
  if (!GetStratumRange(STRATUM_GEN_BEDROCK_BAND, &BandMinY, &BandMaxY) ||
      GlobalY <= BandMinY) {
    return Base->BedrockId;
  }

  const uint64_t MASK16 = 0xFFFFULL;
  const float HASH_SCALE = 65536.0F;
  float Height = (float)(BandMaxY - BandMinY + 1);
  float Probability = 1.0F - ((float)(GlobalY - BandMinY) / Height);

  uint64_t Hash = SeedVal ^ BEDROCK_HASH_SALT;
  Hash = SplitMix64(Hash ^ (uint64_t)(uint32_t)GlobalX);
  Hash = SplitMix64(Hash ^ (uint64_t)(uint32_t)GlobalY);
  Hash = SplitMix64(Hash ^ (uint64_t)(uint32_t)GlobalZ);
  float Roll = (float)(Hash & MASK16) / HASH_SCALE;
  return (Roll < Probability) ? Base->BedrockId : Base->StoneId;
}

static atomic_uint_fast64_t *GetCubeClassCounters(void) {
  static atomic_uint_fast64_t SCounters[CUBE_CLASS_COUNT];
  return SCounters;
}

void GetCubeClassCounts(uint64_t Counts[CUBE_CLASS_COUNT]) {
  atomic_uint_fast64_t *Counters = GetCubeClassCounters();
  for (int Idx = 0; Idx < CUBE_CLASS_COUNT; Idx++) {
    Counts[Idx] = atomic_load_explicit(&Counters[Idx], memory_order_relaxed);
  }
}

// Decides from the strata and the column's surface bounds alone whether a cube
// is entirely air, entirely stone, or needs the per-block path. Every bound is
// conservative: a cube is AIR or SOLID only when the per-block path could not
// produce anything else, which the worldgen self-test checks byte for byte.
static CubeClass ClassifyCube(int BaseY, const StratumGenerator *Layers,
                              const ColumnEntry *Column) {
  int TopY = BaseY + CHUNK_SIZE - 1;
  bool AllVoid = true;
  bool AllOverworld = true;
  for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
    AllVoid &= (Layers[IdxY] == STRATUM_GEN_VOID);
    AllOverworld &= (Layers[IdxY] == STRATUM_GEN_OVERWORLD);
  }

  if (AllVoid) {
    return CUBE_CLASS_AIR;
  }
  if (!AllOverworld) {
    return CUBE_CLASS_MIXED;
  }

  int HighestFilled =
      (Column->MaxSurface > SEA_LEVEL) ? Column->MaxSurface : SEA_LEVEL;
  if (BaseY > HighestFilled) {
    return CUBE_CLASS_AIR;
  }
  if (TopY < Column->MinSurface - GetMaxBiomeSubsurfaceDepth()) {
    return CUBE_CLASS_SOLID;
  }
  return CUBE_CLASS_MIXED;
}

static int FillCubeSolid(Chunk *ChunkVal, BlockID BlockId) {
  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
      for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
        ChunkVal->Data[IdxX][IdxY][IdxZ] = BlockId;
      }
    }
  }
  return (BlockId != BLOCK_AIR) ? (int)CHUNK_VOLUME : 0;
}

static int FillCubePerBlock(Chunk *ChunkVal, const ColumnEntry *Column,
                            const StratumGenerator *Layers, uint64_t SeedVal) {
  int SolidCount = 0;
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;

  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
      int GlobalX = (ChunkVal->ChunkX * CHUNK_SIZE) + IdxX;
      int GlobalZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + IdxZ;
      int TerrainHeight = Column->Surface[IdxX][IdxZ];

      for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
        int GlobalY = BaseY + IdxY;
        BlockID BlockId = BLOCK_AIR;

        switch (Layers[IdxY]) {
        case STRATUM_GEN_OVERWORLD: {
          const BiomeType *Biome =
              GetBiomeDef(GetChunkBiomeAtLocal(ChunkVal, IdxX, IdxY, IdxZ));
          BlockId = DetermineBlockId(Biome, GlobalY, TerrainHeight);
          break;
        }
        case STRATUM_GEN_BEDROCK_BAND:
          BlockId =
              DetermineBedrockBandBlock(GlobalX, GlobalY, GlobalZ, SeedVal);
          break;
        case STRATUM_GEN_VOID:
        case STRATUM_GEN_COUNT:
        default:
          break;
        }

        ChunkVal->Data[IdxX][IdxY][IdxZ] = BlockId;
        if (BlockId != BLOCK_AIR) {
          SolidCount++;
        }
      }
    }
  }
  return SolidCount;
}

void GenerateChunkTerrain(Chunk *ChunkVal, const ColumnEntry *Column) {
  const TerrainGenOptions DEFAULT_OPTIONS = {.AllowClassification = true,
                                             .CountClasses = false};
  (void)GenerateChunkTerrainEx(ChunkVal, Column, &DEFAULT_OPTIONS);
}

// Requires BiomeMap to be filled: the caller runs FillChunkBiomeMap first, so
// that generated and disk-loaded chunks share one fill site.
//
// Each Y layer is generated by the stratum it belongs to. A cube can straddle a
// stratum boundary, so the generator is resolved once per layer, never per block.
CubeClass GenerateChunkTerrainEx(Chunk *ChunkVal, const ColumnEntry *Column,
                                 const TerrainGenOptions *Options) {
  ChunkVal->IsModified = false;

  uint64_t SeedVal = GetWorldSeed();
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  int Ceiling = GetGenerationCeiling();

  StratumGenerator Layers[CHUNK_SIZE];
  for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
    int GlobalY = BaseY + IdxY;
    Layers[IdxY] =
        (GlobalY >= Ceiling) ? STRATUM_GEN_VOID : ResolveStratum(GlobalY);
  }

  CubeClass Class = ClassifyCube(BaseY, Layers, Column);
  int SolidCount = 0;
  if (!Options->AllowClassification || Class == CUBE_CLASS_MIXED) {
    SolidCount = FillCubePerBlock(ChunkVal, Column, Layers, SeedVal);
  } else if (Class == CUBE_CLASS_AIR) {
    SolidCount = FillCubeSolid(ChunkVal, BLOCK_AIR);
  } else {
    SolidCount = FillCubeSolid(ChunkVal, GetTerrainBaseBlocks()->StoneId);
  }

  if (Options->CountClasses) {
    atomic_fetch_add_explicit(&GetCubeClassCounters()[Class], 1,
                              memory_order_relaxed);
  }

  ChunkVal->SolidBlockCount = SolidCount;
  ChunkVal->IsDirty = true;
  return Class;
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

void SetBlockInChunk(Chunk *ChunkVal, Vec3 LocalPos, BlockID BlockId) {
  int IdxX = (int)LocalPos.x;
  int IdxY = (int)LocalPos.y;
  int IdxZ = (int)LocalPos.z;

  if (IdxX >= 0 && IdxX < CHUNK_SIZE && IdxY >= 0 && IdxY < CHUNK_SIZE && IdxZ >= 0 &&
      IdxZ < CHUNK_SIZE) {
    BlockID OldId = ChunkVal->Data[IdxX][IdxY][IdxZ];

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
