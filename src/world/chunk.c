#include "world/chunk.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "core/log.h"
#include "persistence/world_save.h"
#include "core/hash.h"
#include "world/surface.h"
#include "world/cave_fields.h"
#include "world/terrain_density.h"
#include "world/terrain_shape.h"
#include "world/worldgen_column_cache.h"
#include "world/stone_filler.h"
#include "world/worldgen_strata.h"
#include "world/dense_pool.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// Air is structurally 0 throughout the engine: an empty block cell means air, and
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
  BlockID LavaId;
  BlockID IceId;
} TerrainBaseBlocks;

#define BEDROCK_HASH_SALT 0xBEDB0C4000000001ULL

static TerrainBaseBlocks *GetTerrainBaseBlocks(void) {
  static TerrainBaseBlocks SBase = {0, 0, 0, 0, 0};
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
  Base->LavaId = ResolveBaseBlock("Lava");
  Base->IceId = ResolveBaseBlock("ice");
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

BlockID TerrainFluidBlock(int Fluid) {
  const TerrainBaseBlocks *Base = GetTerrainBaseBlocks();
  switch (Fluid) {
  case CAVE_FLUID_WATER:
    return Base->WaterId;
  case CAVE_FLUID_LAVA:
    return Base->LavaId;
  default:
    return BLOCK_AIR;
  }
}

// Palette by solid depth for the fluid and palette layers: Depth is the number
// of contiguous solid blocks directly above (-1 for a non-solid position), and
// deeper blocks (Depth >= SubsurfaceDepth) come from the stone filler instead. Non-solid positions take their
// fluid; the biome supplies the surface block (depth 0) and the subsurface
// layer under it; everything deeper is stone. For a heightmap column this is
// exactly the old height-based rule, and it also gives overhang tops, cliff
// faces, and cave floors their palette layers.
static BlockID DeterminePaletteBlock(const BiomeType *Biome, int Depth,
                                     int GlobalY, int Surface, bool Flooded,
                                     bool Steep) {
  const TerrainBaseBlocks *Base = GetTerrainBaseBlocks();

  if (Depth < 0) {
    CaveFluid Fluid = CaveFluidAt(GlobalY, Surface, Flooded);
    // Flooded cave water never freezes; only the sea surface does.
    if (Fluid == CAVE_FLUID_WATER && !Flooded && GlobalY == SEA_LEVEL &&
        Biome->FrozenWater) {
      return Base->IceId;
    }
    return TerrainFluidBlock((int)Fluid);
  }
  if (Steep && Depth < Biome->SubsurfaceDepth) {
    return (BlockID)Biome->SteepBlockId;
  }
  if (Depth == 0) {
    return (GlobalY <= SEA_LEVEL + 1) ? (BlockID)Biome->UnderwaterBlockId
                                      : (BlockID)Biome->SurfaceBlockId;
  }
  return (BlockID)Biome->SubsurfaceBlockId;
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
      int Floor = Column->CellFloor[CellX][CellZ];

      // The climate depends only on (X, Z) and comes from the column entry, as
      // does the biome for every depth in the surface band; only cells outside
      // the band are resolved here.
      BiomeClimate Climate = Column->Climate[CellX][CellZ];
      for (int CellY = 0; CellY < BIOME_CELLS_PER_CHUNK; CellY++) {
        int GlobalY = (ChunkVal->ChunkY * CHUNK_SIZE) +
                      (CellY * BIOME_CELL_SIZE) + CELL_CENTER;
        int Depth = Floor - GlobalY;
        int BiomeId = Column->SurfaceBiome[CellX][CellZ];
        if (!BiomeInSurfaceBand(Depth)) {
          Climate.Depth = (float)Depth;
          BiomeId = ResolveBiome(&Climate);
        }
        ChunkVal->BiomeMap[CellX][CellY][CellZ] = (unsigned char)BiomeId;
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
      float Shell = Column->CornerShell[CornerX][CornerZ];
      for (int CornerY = 0; CornerY < DENSITY_CORNERS_Y; CornerY++) {
        Out->Values[CornerX][CornerY][CornerZ] = TerrainCornerDensity(
            Base, Variation, Shell, OriginX + (CornerX * DENSITY_CELL_XZ),
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
  // Caves can open anywhere between the shell and the cave floor, so the bound
  // alone proves solidity only below the floor or with caves disabled.
  bool CaveFree = !CavesEnabled() || PaddedTop < GetCaveParams()->CaveFloor;
  if (CaveFree && (float)PaddedTop < Column->BoundLow &&
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

  if (AllNonSolid && BaseY > SEA_LEVEL && BaseY > GetCaveParams()->LavaLevel) {
    return CUBE_CLASS_AIR;
  }
  if (AllSolid && BaseY + CHUNK_SIZE - 1 + DENSITY_TOP_PADDING < Ceiling) {
    return CUBE_CLASS_SOLID;
  }
  return CUBE_CLASS_MIXED;
}

// Whether the terrain at a block is steeper than the biome steep threshold:
// the analytic gradient of its cell's trilinear blend, horizontal over
// vertical. Reads only the cube's shared corners, so the decision is the same
// on both sides of a cube boundary.
static bool IsSteepAt(const CubeCorners *Corners, int LocalX, int LocalY,
                      int LocalZ) {
  int Cx = LocalX / DENSITY_CELL_XZ;
  int Cy = LocalY / DENSITY_CELL_Y;
  int Cz = LocalZ / DENSITY_CELL_XZ;
  float Fx = (float)(LocalX % DENSITY_CELL_XZ) / (float)DENSITY_CELL_XZ;
  float Fy = (float)(LocalY % DENSITY_CELL_Y) / (float)DENSITY_CELL_Y;
  float Fz = (float)(LocalZ % DENSITY_CELL_XZ) / (float)DENSITY_CELL_XZ;
  const float (*V)[DENSITY_CORNERS_Y][DENSITY_CORNERS_XZ] = Corners->Values;
  float C0 = V[Cx][Cy][Cz];
  float C1 = V[Cx + 1][Cy][Cz];
  float C2 = V[Cx][Cy + 1][Cz];
  float C3 = V[Cx + 1][Cy + 1][Cz];
  float C4 = V[Cx][Cy][Cz + 1];
  float C5 = V[Cx + 1][Cy][Cz + 1];
  float C6 = V[Cx][Cy + 1][Cz + 1];
  float C7 = V[Cx + 1][Cy + 1][Cz + 1];
  float Gx = (((1.0F - Fy) * (1.0F - Fz) * (C1 - C0)) +
              (Fy * (1.0F - Fz) * (C3 - C2)) + ((1.0F - Fy) * Fz * (C5 - C4)) +
              (Fy * Fz * (C7 - C6))) /
             (float)DENSITY_CELL_XZ;
  float Gy = (((1.0F - Fx) * (1.0F - Fz) * (C2 - C0)) +
              (Fx * (1.0F - Fz) * (C3 - C1)) + ((1.0F - Fx) * Fz * (C6 - C4)) +
              (Fx * Fz * (C7 - C5))) /
             (float)DENSITY_CELL_Y;
  float Gz = (((1.0F - Fx) * (1.0F - Fy) * (C4 - C0)) +
              (Fx * (1.0F - Fy) * (C5 - C1)) + ((1.0F - Fx) * Fy * (C6 - C2)) +
              (Fx * Fy * (C7 - C3))) /
             (float)DENSITY_CELL_XZ;
  return hypotf(Gx, Gz) > GetBiomeParams()->SteepSlope * fabsf(Gy);
}

// Flood field at the 2 x 3 x 2 lattice points around a cube (its bottom, top,
// and the top of the padding rows), loaded on first use: most cubes never need
// it.
typedef struct {
  bool Loaded;
  float Values[2][3][2];
} CubeFlood;

static void LoadCubeFlood(const Chunk *ChunkVal, uint64_t SeedVal,
                          CubeFlood *Flood) {
  int OriginX = ChunkVal->ChunkX * CHUNK_SIZE;
  int OriginY = ChunkVal->ChunkY * CHUNK_SIZE;
  int OriginZ = ChunkVal->ChunkZ * CHUNK_SIZE;
  for (int Ix = 0; Ix < 2; Ix++) {
    for (int Iy = 0; Iy < 3; Iy++) {
      for (int Iz = 0; Iz < 2; Iz++) {
        Flood->Values[Ix][Iy][Iz] = CaveFloodLattice(
            OriginX + (Ix * CAVE_FLOOD_CELL), OriginY + (Iy * CAVE_FLOOD_CELL),
            OriginZ + (Iz * CAVE_FLOOD_CELL), SeedVal);
      }
    }
  }
  Flood->Loaded = true;
}


static int FillCubeSolid(Chunk *ChunkVal, BlockID BlockId);
static int FillCubeUniform(Chunk *ChunkVal, BlockID BlockId);

// A SOLID cube is filler throughout: every block has at least 8 solid blocks
// above it, deeper than any palette layer. One fill when the filler is uniform
// over the cube, otherwise the same per-block decision the per-block path makes.
// Returns -1 when no block buffer could be leased.
static int FillCubeFiller(Chunk *ChunkVal, uint64_t SeedVal) {
  FillerCube Filler;
  FillerCubeInit(&Filler, ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ,
                 SeedVal);
  BlockID Uniform = 0;
  if (FillerCubeUniform(&Filler, &Uniform)) {
    return FillCubeUniform(ChunkVal, Uniform);
  }
  if (!ChunkBeginFill(ChunkVal)) {
    return -1;
  }
  FillerFillCube(&Filler, ChunkVal->ChunkX * CHUNK_SIZE,
                 ChunkVal->ChunkZ * CHUNK_SIZE, *ChunkVal->Blocks);
  return (int)CHUNK_VOLUME;
}

static int FillCubeSolid(Chunk *ChunkVal, BlockID BlockId) {
  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
      for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
        (*ChunkVal->Blocks)[IdxX][IdxY][IdxZ] = BlockId;
      }
    }
  }
  return (BlockId != BLOCK_AIR) ? (int)CHUNK_VOLUME : 0;
}

// A pooled chunk takes the uniform result as scalar storage and leases nothing.
// A standalone chunk keeps the buffer its caller attached, so it is filled as
// before and tests can still compare it cell by cell.
static int FillCubeUniform(Chunk *ChunkVal, BlockID BlockId) {
  if (ChunkVal->Pool == (struct DenseBlockPool *)0 &&
      ChunkVal->Blocks != (DenseBlockBuffer *)0) {
    return FillCubeSolid(ChunkVal, BlockId);
  }
  ChunkResetStorage(ChunkVal, BlockId);
  return (BlockId != BLOCK_AIR) ? (int)CHUNK_VOLUME : 0;
}

enum {
  PADDED_ROWS = CHUNK_SIZE + DENSITY_TOP_PADDING
};

// One block column of a cube being filled.
typedef struct {
  const CubeCorners *Corners;
  CubeFlood *Flood;
  const Chunk *ChunkVal;
  uint64_t SeedVal;
  int LocalX;
  int LocalZ;
  int Surface;
  float *DebugColumn;
} ColumnFill;

// Open rows in the flood zone become barrier rock near the flood threshold and
// are flagged flooded above it. Flooded must arrive all false.
//
// The flood value is the trilinear blend of the cube's flood lattice, written
// out like InterpolateColumnDensity: the X blends of each lattice row are done
// once per column, then each row performs the Y and Z steps. Operations and
// their order are TrilinearDensity's, so the probe gets the same float.
static void ApplyFloodBarrier(const ColumnFill *Fill, int BaseY,
                              float Density[PADDED_ROWS],
                              bool Flooded[PADDED_ROWS]) {
  int Lowest = 0;
  int Highest = 0;
  if (!CaveFloodRows(Fill->Surface, &Lowest, &Highest)) {
    return;
  }
  int First = (Lowest > BaseY) ? Lowest - BaseY : 0;
  int Last = (Highest < BaseY + PADDED_ROWS - 1) ? Highest - BaseY
                                                  : PADDED_ROWS - 1;
  bool Blended = false;
  float NearZ[3];
  float FarZ[3];
  float Threshold = GetCaveParams()->FloodThreshold;
  for (int Row = First; Row <= Last; Row++) {
    if (Density[Row] > 0.0F) {
      continue;
    }
    if (!Blended) {
      if (!Fill->Flood->Loaded) {
        LoadCubeFlood(Fill->ChunkVal, Fill->SeedVal, Fill->Flood);
      }
      const float (*V)[3][2] = Fill->Flood->Values;
      float Fx = (float)Fill->LocalX / (float)CAVE_FLOOD_CELL;
      for (int Level = 0; Level < 3; Level++) {
        NearZ[Level] = V[0][Level][0] + ((V[1][Level][0] - V[0][Level][0]) * Fx);
        FarZ[Level] = V[0][Level][1] + ((V[1][Level][1] - V[0][Level][1]) * Fx);
      }
      Blended = true;
    }
    int CellY = Row / CAVE_FLOOD_CELL;
    float Fy = (float)(Row % CAVE_FLOOD_CELL) / (float)CAVE_FLOOD_CELL;
    float Fz = (float)Fill->LocalZ / (float)CAVE_FLOOD_CELL;
    float Y0 = NearZ[CellY] + ((NearZ[CellY + 1] - NearZ[CellY]) * Fy);
    float Y1 = FarZ[CellY] + ((FarZ[CellY + 1] - FarZ[CellY]) * Fy);
    float Flood = Y0 + ((Y1 - Y0) * Fz);
    float Barrier = CaveBarrierDensity(Flood);
    if (Barrier > 0.0F) {
      Density[Row] = Barrier;
    } else {
      Flooded[Row] = Flood > Threshold;
    }
  }
}

// Solid depth of every row of one block column, padding included. The row above
// the last padding row is unknown and treated as non-solid; that can only
// shorten a depth to 8 or more, which every biome already renders as stone.
static void ComputeColumnDepths(const ColumnFill *Fill, int BaseY, int Ceiling,
                                int Depths[PADDED_ROWS],
                                bool Flooded[PADDED_ROWS]) {
  bool Solid[PADDED_ROWS];
  float Density[PADDED_ROWS];
  InterpolateColumnDensity(Fill->Corners, Fill->LocalX, Fill->LocalZ, Density);
  ApplyFloodBarrier(Fill, BaseY, Density, Flooded);
  float *DebugColumn = Fill->DebugColumn;
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
static int FillCubePerBlock(Chunk *ChunkVal, const ColumnEntry *Column,
                            const CubeCorners *Corners,
                            const StratumGenerator *Layers, uint64_t SeedVal,
                            float *DebugDensity) {
  int SolidCount = 0;
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  int Ceiling = GetGenerationCeiling();
  CubeFlood Flood = {.Loaded = false};
  // Built on the first block deeper than the palette layers.
  FillerCube Filler;
  bool HaveFiller = false;

  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
      int GlobalX = (ChunkVal->ChunkX * CHUNK_SIZE) + IdxX;
      int GlobalZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + IdxZ;

      int Depths[PADDED_ROWS] = {0};
      bool Flooded[PADDED_ROWS] = {false};
      if (Corners != NULL) {
        ColumnFill Fill = {
            .Corners = Corners,
            .Flood = &Flood,
            .ChunkVal = ChunkVal,
            .SeedVal = SeedVal,
            .LocalX = IdxX,
            .LocalZ = IdxZ,
            .Surface = Column->Surface[IdxX][IdxZ],
            .DebugColumn =
                (DebugDensity != NULL)
                    ? &DebugDensity[((size_t)IdxX * CHUNK_SIZE * CHUNK_SIZE) +
                                    (size_t)IdxZ]
                    : NULL};
        ComputeColumnDepths(&Fill, BaseY, Ceiling, Depths, Flooded);
      }

      for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
        int GlobalY = BaseY + IdxY;
        BlockID BlockId = BLOCK_AIR;

        switch (Layers[IdxY]) {
        case STRATUM_GEN_OVERWORLD: {
          const BiomeType *Biome =
              GetBiomeDef(GetChunkBiomeAtLocal(ChunkVal, IdxX, IdxY, IdxZ));
          int Depth = Depths[IdxY];
          if (Depth >= Biome->SubsurfaceDepth) {
            if (!HaveFiller) {
              FillerCubeInit(&Filler, ChunkVal->ChunkX, ChunkVal->ChunkY,
                             ChunkVal->ChunkZ, SeedVal);
              HaveFiller = true;
            }
            BlockId = FillerBlockAt(&Filler, GlobalX, GlobalY, GlobalZ);
            break;
          }
          bool Steep = Corners != NULL && Biome->SteepBlockId >= 0 &&
                       Depth >= 0 && Depth < Biome->SubsurfaceDepth &&
                       IsSteepAt(Corners, IdxX, IdxY, IdxZ);
          BlockId = DeterminePaletteBlock(Biome, Depth, GlobalY,
                                          Column->Surface[IdxX][IdxZ],
                                          Flooded[IdxY], Steep);
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

        (*ChunkVal->Blocks)[IdxX][IdxY][IdxZ] = BlockId;
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
    SolidCount = ChunkBeginFill(ChunkVal)
                     ? FillCubePerBlock(ChunkVal, Column,
                                        HaveCorners ? &Corners : NULL, Layers,
                                        SeedVal, Options->DebugDensity)
                     : -1;
  } else if (Class == CUBE_CLASS_AIR) {
    // Sky and void cubes: no noise sampled, no cell written, no buffer leased.
    SolidCount = FillCubeUniform(ChunkVal, BLOCK_AIR);
  } else {
    SolidCount = FillCubeFiller(ChunkVal, SeedVal);
  }

  if (SolidCount < 0) {
    LogError("CHUNK: no block buffer to generate (%d, %d, %d) into",
             ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
    ChunkResetStorage(ChunkVal, BLOCK_AIR);
    SolidCount = 0;
  }

  if (Options->CountClasses) {
    atomic_fetch_add_explicit(&GetCubeClassCounters()[Class], 1,
                              memory_order_relaxed);
  }

  ChunkVal->SolidBlockCount = SolidCount;
  ChunkVal->IsDirty = true;
  return Class;
}

void ChunkAttachBuffer(Chunk *ChunkVal, DenseBlockBuffer *Buffer) {
  ChunkVal->Pool = (struct DenseBlockPool *)0;
  ChunkVal->Blocks = Buffer;
  ChunkVal->IsUniform = false;
}

Chunk *ChunkCreateStandalone(void) {
  Chunk *ChunkVal = (Chunk *)calloc(1, sizeof(Chunk));
  DenseBlockBuffer *Buffer =
      (DenseBlockBuffer *)calloc(1, sizeof(DenseBlockBuffer));
  if (ChunkVal == (Chunk *)0 || Buffer == (DenseBlockBuffer *)0) {
    free(ChunkVal);
    free(Buffer);
    return (Chunk *)0;
  }
  ChunkAttachBuffer(ChunkVal, Buffer);
  return ChunkVal;
}

void ChunkFreeStandalone(Chunk *ChunkVal) {
  if (ChunkVal == (Chunk *)0) {
    return;
  }
  free(ChunkVal->Blocks);
  free(ChunkVal);
}

void ChunkResetStorage(Chunk *ChunkVal, BlockID BlockId) {
  if (ChunkVal->Blocks != (DenseBlockBuffer *)0 &&
      ChunkVal->Pool != (struct DenseBlockPool *)0) {
    DensePoolRelease(ChunkVal->Pool, ChunkVal->Blocks);
  }
  ChunkVal->Blocks = (DenseBlockBuffer *)0;
  ChunkVal->UniformBlockId = BlockId;
  ChunkVal->IsUniform = true;
}

bool ChunkBeginFill(Chunk *ChunkVal) {
  if (ChunkVal->Blocks == (DenseBlockBuffer *)0) {
    if (ChunkVal->Pool == (struct DenseBlockPool *)0) {
      return false;
    }
    ChunkVal->Blocks = DensePoolAcquire(ChunkVal->Pool);
    if (ChunkVal->Blocks == (DenseBlockBuffer *)0) {
      return false;
    }
  }
  ChunkVal->IsUniform = false;
  return true;
}

void ChunkEndFill(Chunk *ChunkVal) {
  if (ChunkVal->Blocks == (DenseBlockBuffer *)0 ||
      ChunkVal->Pool == (struct DenseBlockPool *)0) {
    return;
  }
  const BlockID *Cells = &(*ChunkVal->Blocks)[0][0][0];
  BlockID First = Cells[0];
  for (size_t Idx = 1; Idx < CHUNK_VOLUME; Idx++) {
    if (Cells[Idx] != First) {
      return;
    }
  }
  ChunkResetStorage(ChunkVal, First);
}

bool PromoteChunkToDense(Chunk *ChunkVal) {
  if (ChunkVal->Blocks != (DenseBlockBuffer *)0) {
    return true;
  }
  if (ChunkVal->Pool == (struct DenseBlockPool *)0) {
    return false;
  }
  DenseBlockBuffer *Buffer = DensePoolAcquire(ChunkVal->Pool);
  if (Buffer == (DenseBlockBuffer *)0) {
    return false;
  }
  BlockID *Cells = &(*Buffer)[0][0][0];
  for (size_t Idx = 0; Idx < CHUNK_VOLUME; Idx++) {
    Cells[Idx] = ChunkVal->UniformBlockId;
  }
  ChunkVal->Blocks = Buffer;
  ChunkVal->IsUniform = false;
  return true;
}

int GetBlockIdInChunk(Chunk *ChunkVal, Vec3 LocalPos) {
  int IdxX = (int)LocalPos.x;
  int IdxY = (int)LocalPos.y;
  int IdxZ = (int)LocalPos.z;

  if (IdxX < 0 || IdxX >= CHUNK_SIZE || IdxY < 0 || IdxY >= CHUNK_SIZE || IdxZ < 0 ||
      IdxZ >= CHUNK_SIZE) {
    return 0;
  }

  return (int)ChunkGetLocal(ChunkVal, IdxX, IdxY, IdxZ);
}

void SetBlockInChunk(Chunk *ChunkVal, Vec3 LocalPos, BlockID BlockId) {
  int IdxX = (int)LocalPos.x;
  int IdxY = (int)LocalPos.y;
  int IdxZ = (int)LocalPos.z;

  if (IdxX >= 0 && IdxX < CHUNK_SIZE && IdxY >= 0 && IdxY < CHUNK_SIZE && IdxZ >= 0 &&
      IdxZ < CHUNK_SIZE) {
    BlockID OldId = ChunkGetLocal(ChunkVal, IdxX, IdxY, IdxZ);

    if (OldId != BlockId) {
      if (!PromoteChunkToDense(ChunkVal)) {
        LogError("CHUNK: no block buffer to promote (%d, %d, %d); edit dropped",
                 ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
        return;
      }
      if (OldId == 0 && BlockId != 0) {
        ChunkVal->SolidBlockCount++;
      } else if (OldId != 0 && BlockId == 0) {
        ChunkVal->SolidBlockCount--;
      }

      (*ChunkVal->Blocks)[IdxX][IdxY][IdxZ] = BlockId;
      ChunkVal->IsDirty = true;
      ChunkVal->IsModified = true;
    }
  }
}
