#include "world/chunk.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "core/log.h"
#include "persistence/world_save.h"
#include "core/hash.h"
#include "world/surface.h"
#include "world/terrain_density.h"
#include "world/terrain_shape.h"
#include "world/worldgen_column_cache.h"
#include "world/worldgen_strata.h"
#include <math.h>
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

// Palette by solid depth: Depth is the number of contiguous solid blocks
// directly above (-1 for a non-solid position). Air or water fills non-solid
// positions; the biome supplies the surface block (depth 0) and the subsurface
// layer under it; everything deeper is stone. For a heightmap column this is
// exactly the old height-based rule, and it also gives overhang tops and cliff
// faces their palette layers.
static BlockID DeterminePaletteBlock(const BiomeType *Biome, int Depth,
                                     int GlobalY) {
  const TerrainBaseBlocks *Base = GetTerrainBaseBlocks();

  if (Depth < 0) {
    return (GlobalY <= SEA_LEVEL) ? Base->WaterId : BLOCK_AIR;
  }
  if (Depth == 0) {
    return (GlobalY <= SEA_LEVEL + 1) ? (BlockID)Biome->UnderwaterBlockId
                                      : (BlockID)Biome->SurfaceBlockId;
  }
  if (Depth < Biome->SubsurfaceDepth) {
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

// Density at every grid corner a cube needs: its own 5 x 3 x 5 corners plus the
// row 8 blocks above its top.
typedef struct {
  float Values[DENSITY_CORNERS_XZ][DENSITY_CORNERS_Y][DENSITY_CORNERS_XZ];
} CubeCorners;

static void EvaluateCubeCorners(const Chunk *ChunkVal,
                                const ColumnEntry *Column, uint64_t SeedVal,
                                CubeCorners *Out) {
  int OriginX = ChunkVal->ChunkX * CHUNK_SIZE;
  int OriginY = ChunkVal->ChunkY * CHUNK_SIZE;
  int OriginZ = ChunkVal->ChunkZ * CHUNK_SIZE;
  for (int CornerX = 0; CornerX < DENSITY_CORNERS_XZ; CornerX++) {
    for (int CornerZ = 0; CornerZ < DENSITY_CORNERS_XZ; CornerZ++) {
      float Base = Column->CornerBase[CornerX][CornerZ];
      float Variation = Column->CornerVariation[CornerX][CornerZ];
      for (int CornerY = 0; CornerY < DENSITY_CORNERS_Y; CornerY++) {
        Out->Values[CornerX][CornerY][CornerZ] = TerrainCornerDensity(
            Base, Variation, OriginX + (CornerX * DENSITY_CELL_XZ),
            OriginY + (CornerY * DENSITY_CELL_Y),
            OriginZ + (CornerZ * DENSITY_CELL_XZ), SeedVal);
      }
    }
  }
}

// Interpolated density down one block column, padding rows included.
//
// TrilinearDensity blends along X first, then Y, then Z. Within a block column
// X and Z are fixed, so the X blends of every corner row are computed once here
// and each block only performs the Y and Z steps. The operations, operands, and
// their order are exactly TrilinearDensity's, so the result is bit-identical to
// SampleTerrainDensity (the worldgen self-test checks it).
static void InterpolateColumnDensity(const CubeCorners *Corners, int LocalX,
                                     int LocalZ, float Out[]) {
  int CellX = LocalX / DENSITY_CELL_XZ;
  int CellZ = LocalZ / DENSITY_CELL_XZ;
  float Fx = (float)(LocalX % DENSITY_CELL_XZ) / (float)DENSITY_CELL_XZ;
  float Fz = (float)(LocalZ % DENSITY_CELL_XZ) / (float)DENSITY_CELL_XZ;

  float NearZ[DENSITY_CORNERS_Y];
  float FarZ[DENSITY_CORNERS_Y];
  for (int Row = 0; Row < DENSITY_CORNERS_Y; Row++) {
    float C0 = Corners->Values[CellX][Row][CellZ];
    float C1 = Corners->Values[CellX + 1][Row][CellZ];
    float C4 = Corners->Values[CellX][Row][CellZ + 1];
    float C5 = Corners->Values[CellX + 1][Row][CellZ + 1];
    NearZ[Row] = C0 + ((C1 - C0) * Fx);
    FarZ[Row] = C4 + ((C5 - C4) * Fx);
  }

  for (int LocalY = 0; LocalY < CHUNK_SIZE + DENSITY_TOP_PADDING; LocalY++) {
    int CellY = LocalY / DENSITY_CELL_Y;
    float Fy = (float)(LocalY % DENSITY_CELL_Y) / (float)DENSITY_CELL_Y;
    float Y0 = NearZ[CellY] + ((NearZ[CellY + 1] - NearZ[CellY]) * Fy);
    float Y1 = FarZ[CellY] + ((FarZ[CellY + 1] - FarZ[CellY]) * Fy);
    Out[LocalY] = Y0 + ((Y1 - Y0) * Fz);
  }
}

// Tier 2: the column's density bounds alone, no 3D noise. Returns MIXED when
// they cannot decide.
static CubeClass ClassifyByBounds(int BaseY, const ColumnEntry *Column,
                                  int Ceiling) {
  int PaddedTop = BaseY + CHUNK_SIZE - 1 + DENSITY_TOP_PADDING;
  float HighestSolid = (Column->BoundHigh > (float)SEA_LEVEL)
                           ? Column->BoundHigh
                           : (float)SEA_LEVEL;
  if ((float)BaseY > HighestSolid) {
    return CUBE_CLASS_AIR;
  }
  if ((float)PaddedTop < Column->BoundLow &&
      (float)PaddedTop < (float)Ceiling - GetCeilingTaper()) {
    return CUBE_CLASS_SOLID;
  }
  return CUBE_CLASS_MIXED;
}

// Tier 3: trilinear blending never leaves the range of a cell's corners, so the
// corner signs decide exactly. AIR needs every corner of the cube's own rows
// non-positive and no water; SOLID needs every corner, padding row included,
// positive, which gives every block at least 8 solid blocks above it, deeper
// than any biome's palette layers.
static CubeClass ClassifyByCorners(int BaseY, const CubeCorners *Corners,
                                   int Ceiling) {
  bool AllNonSolid = true;
  bool AllSolid = true;
  for (int CornerX = 0; CornerX < DENSITY_CORNERS_XZ; CornerX++) {
    for (int CornerY = 0; CornerY < DENSITY_CORNERS_Y; CornerY++) {
      for (int CornerZ = 0; CornerZ < DENSITY_CORNERS_XZ; CornerZ++) {
        float Density = Corners->Values[CornerX][CornerY][CornerZ];
        AllSolid &= (Density > 0.0F);
        if (CornerY < DENSITY_CORNERS_Y - 1) {
          AllNonSolid &= (Density <= 0.0F);
        }
      }
    }
  }

  if (AllNonSolid && BaseY > SEA_LEVEL) {
    return CUBE_CLASS_AIR;
  }
  if (AllSolid && BaseY + CHUNK_SIZE - 1 + DENSITY_TOP_PADDING < Ceiling) {
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

enum {
  PADDED_ROWS = CHUNK_SIZE + DENSITY_TOP_PADDING
};

// Solid depth of every row of one block column, padding included. The row above
// the last padding row is unknown and treated as non-solid; that can only
// shorten a depth to 8 or more, which every biome already renders as stone.
static void ComputeColumnDepths(const CubeCorners *Corners, int LocalX,
                                int LocalZ, int BaseY, int Ceiling,
                                float *DebugColumn, int Depths[PADDED_ROWS]) {
  bool Solid[PADDED_ROWS];
  float Density[PADDED_ROWS];
  InterpolateColumnDensity(Corners, LocalX, LocalZ, Density);
  for (int Row = 0; Row < PADDED_ROWS; Row++) {
    Solid[Row] = (Density[Row] > 0.0F) && (BaseY + Row < Ceiling);
    if (DebugColumn != NULL && Row < CHUNK_SIZE) {
      DebugColumn[(size_t)Row * CHUNK_SIZE] = Density[Row];
    }
  }

  Depths[PADDED_ROWS - 1] = Solid[PADDED_ROWS - 1] ? 0 : -1;
  for (int Row = PADDED_ROWS - 2; Row >= 0; Row--) {
    if (!Solid[Row]) {
      Depths[Row] = -1;
    } else {
      Depths[Row] = Solid[Row + 1] ? Depths[Row + 1] + 1 : 0;
    }
  }
}

// Corners is NULL when the cube has no overworld layer.
static int FillCubePerBlock(Chunk *ChunkVal, const CubeCorners *Corners,
                            const StratumGenerator *Layers, uint64_t SeedVal,
                            float *DebugDensity) {
  int SolidCount = 0;
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  int Ceiling = GetGenerationCeiling();

  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
      int GlobalX = (ChunkVal->ChunkX * CHUNK_SIZE) + IdxX;
      int GlobalZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + IdxZ;

      int Depths[PADDED_ROWS] = {0};
      if (Corners != NULL) {
        float *DebugColumn =
            (DebugDensity != NULL)
                ? &DebugDensity[((size_t)IdxX * CHUNK_SIZE * CHUNK_SIZE) +
                                (size_t)IdxZ]
                : NULL;
        ComputeColumnDepths(Corners, IdxX, IdxZ, BaseY, Ceiling, DebugColumn,
                            Depths);
      }

      for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
        int GlobalY = BaseY + IdxY;
        BlockID BlockId = BLOCK_AIR;

        switch (Layers[IdxY]) {
        case STRATUM_GEN_OVERWORLD: {
          const BiomeType *Biome =
              GetBiomeDef(GetChunkBiomeAtLocal(ChunkVal, IdxX, IdxY, IdxZ));
          BlockId = DeterminePaletteBlock(Biome, Depths[IdxY], GlobalY);
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

static void ClearDebugDensity(float *DebugDensity) {
  if (DebugDensity == NULL) {
    return;
  }
  for (size_t Idx = 0; Idx < CHUNK_VOLUME; Idx++) {
    DebugDensity[Idx] = NAN;
  }
}

void GenerateChunkTerrain(Chunk *ChunkVal, const ColumnEntry *Column) {
  const TerrainGenOptions DEFAULT_OPTIONS = {.AllowClassification = true,
                                             .CountClasses = false,
                                             .DebugDensity = NULL};
  (void)GenerateChunkTerrainEx(ChunkVal, Column, &DEFAULT_OPTIONS);
}

// Requires BiomeMap to be filled: the caller runs FillChunkBiomeMap first, so
// that generated and disk-loaded chunks share one fill site.
//
// Each Y layer is generated by the stratum it belongs to. A cube can straddle a
// stratum boundary, so the generator is resolved once per layer, never per block.
// Classification runs in tiers (strata, then column bounds, then corner signs);
// every short-circuit gives exactly what the per-block path would.
CubeClass GenerateChunkTerrainEx(Chunk *ChunkVal, const ColumnEntry *Column,
                                 const TerrainGenOptions *Options) {
  ChunkVal->IsModified = false;

  uint64_t SeedVal = GetWorldSeed();
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  int Ceiling = GetGenerationCeiling();

  StratumGenerator Layers[CHUNK_SIZE];
  bool AllVoid = true;
  bool AllOverworld = true;
  bool AnyOverworld = false;
  for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
    int GlobalY = BaseY + IdxY;
    Layers[IdxY] =
        (GlobalY >= Ceiling) ? STRATUM_GEN_VOID : ResolveStratum(GlobalY);
    AllVoid &= (Layers[IdxY] == STRATUM_GEN_VOID);
    AllOverworld &= (Layers[IdxY] == STRATUM_GEN_OVERWORLD);
    AnyOverworld |= (Layers[IdxY] == STRATUM_GEN_OVERWORLD);
  }

  CubeCorners Corners;
  bool HaveCorners = false;
  CubeClass Class = CUBE_CLASS_MIXED;
  if (AllVoid) {
    Class = CUBE_CLASS_AIR;
  } else if (AllOverworld) {
    Class = ClassifyByBounds(BaseY, Column, Ceiling);
    if (Class == CUBE_CLASS_MIXED) {
      EvaluateCubeCorners(ChunkVal, Column, SeedVal, &Corners);
      HaveCorners = true;
      Class = ClassifyByCorners(BaseY, &Corners, Ceiling);
    }
  }

  ClearDebugDensity(Options->DebugDensity);
  int SolidCount = 0;
  if (!Options->AllowClassification || Class == CUBE_CLASS_MIXED) {
    if (AnyOverworld && !HaveCorners) {
      EvaluateCubeCorners(ChunkVal, Column, SeedVal, &Corners);
      HaveCorners = true;
    }
    SolidCount = FillCubePerBlock(ChunkVal, HaveCorners ? &Corners : NULL,
                                  Layers, SeedVal, Options->DebugDensity);
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
