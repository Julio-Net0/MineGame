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
#include <emmintrin.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

BlockID TerrainStoneBlockId(void) { return GetTerrainBaseBlocks()->StoneId; }

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

BlockID TerrainIceBlockId(void) { return GetTerrainBaseBlocks()->IceId; }

BlockID TerrainPaletteBlock(const BiomeType *Biome, int Depth, int GlobalY,
                            bool Steep) {
  bool SteepLayer = Steep && Biome->SteepBlockId >= 0;
  return DeterminePaletteBlock(Biome, Depth, GlobalY, 0, false, SteepLayer);
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

static atomic_uint_fast64_t *GetCornerPlaneCounters(void) {
  // Reused, then evaluated.
  static atomic_uint_fast64_t SCounters[2];
  return SCounters;
}

void GetCornerPlaneCounts(uint64_t *OutReused, uint64_t *OutEvaluated) {
  atomic_uint_fast64_t *Counters = GetCornerPlaneCounters();
  *OutReused = atomic_load_explicit(&Counters[0], memory_order_relaxed);
  *OutEvaluated = atomic_load_explicit(&Counters[1], memory_order_relaxed);
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

// Plane by plane: a plane in Rows is copied, any other is evaluated and kept
// there for the cubes above and below. Returns how many planes were copied.
static int EvaluateCubeCorners(const Chunk *ChunkVal,
                               const ColumnEntry *Column, uint64_t SeedVal,
                               CornerRowRing *Rows, CubeCorners *Out) {
  int OriginX = ChunkVal->ChunkX * CHUNK_SIZE;
  int OriginY = ChunkVal->ChunkY * CHUNK_SIZE;
  int OriginZ = ChunkVal->ChunkZ * CHUNK_SIZE;
  int Reused = 0;
  for (int CornerY = 0; CornerY < DENSITY_CORNERS_Y; CornerY++) {
    int PlaneY = OriginY + (CornerY * DENSITY_CELL_Y);
    float Plane[DENSITY_CORNERS_XZ][DENSITY_CORNERS_XZ];
    bool Copied = Rows != (CornerRowRing *)0 &&
                  CornerRowRingLoad(Rows, PlaneY, Plane);
    for (int CornerX = 0; CornerX < DENSITY_CORNERS_XZ; CornerX++) {
      for (int CornerZ = 0; CornerZ < DENSITY_CORNERS_XZ; CornerZ++) {
        if (!Copied) {
          Plane[CornerX][CornerZ] = TerrainCornerDensity(
              Column->CornerBase[CornerX][CornerZ],
              Column->CornerVariation[CornerX][CornerZ],
              Column->CornerShell[CornerX][CornerZ],
              OriginX + (CornerX * DENSITY_CELL_XZ), PlaneY,
              OriginZ + (CornerZ * DENSITY_CELL_XZ), SeedVal);
        }
        Out->Values[CornerX][CornerY][CornerZ] = Plane[CornerX][CornerZ];
      }
    }
    if (Copied) {
      Reused++;
    } else if (Rows != (CornerRowRing *)0) {
      CornerRowRingStore(Rows, PlaneY, Plane);
    }
  }
  return Reused;
}

// Whether a cube none of whose blocks is solid is sea water throughout. Wholly
// below sea level, no block takes the frozen sea surface; at or above every
// column's surface minus the shell (the comparison CaveFluidAt makes), every
// block is sea water; and the flood rows end below that line, so no flood
// barrier rock reaches the cube or its padding.
static bool CubeIsSubmerged(int BaseY, const ColumnEntry *Column) {
  return BaseY + CHUNK_SIZE - 1 < SEA_LEVEL &&
         (float)BaseY >=
             (float)Column->MaxSurface - GetCaveParams()->ShellBase;
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
  if ((float)BaseY > Column->BoundHigh && CubeIsSubmerged(BaseY, Column)) {
    return CUBE_CLASS_WATER;
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
// non-positive and no water; WATER the same corners with the cube submerged;
// SOLID needs every corner, padding row included, positive, which gives every
// block at least 8 solid blocks above it, deeper than any biome's palette
// layers.
static CubeClass ClassifyByCorners(int BaseY, const ColumnEntry *Column,
                                   const CubeCorners *Corners, int Ceiling) {
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
  if (AllNonSolid && CubeIsSubmerged(BaseY, Column)) {
    return CUBE_CLASS_WATER;
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
  PADDED_ROWS = CHUNK_SIZE + DENSITY_TOP_PADDING,
  SIMD_LANES = 4,
  CELLS_PER_SLICE = CHUNK_SIZE / DENSITY_CELL_XZ
};

// The slice routines give each vector lane one density cell along Z, and fill
// a cell's block columns from one lane, so both counts must match the lanes.
_Static_assert(DENSITY_CELL_XZ == SIMD_LANES,
               "a density cell must span one vector of block columns");
_Static_assert(CELLS_PER_SLICE == SIMD_LANES,
               "a cube slice must span one vector of density cells");

// Every block column of one X slice of a cube, indexed [Row][LocalZ].
typedef struct {
  _Alignas(__m128) float Density[PADDED_ROWS][CHUNK_SIZE];
  _Alignas(__m128i) int Depths[PADDED_ROWS][CHUNK_SIZE];
  bool Flooded[PADDED_ROWS][CHUNK_SIZE];
} ColumnSlice;

// One X slice of a cube being filled.
typedef struct {
  const CubeCorners *Corners;
  CubeFlood *Flood;
  const Chunk *ChunkVal;
  const ColumnEntry *Column;
  uint64_t SeedVal;
  int LocalX;
  int BaseY;
  int Ceiling;
} SliceFill;

// Interpolated density of every block column of an X slice, padding rows
// included.
//
// TrilinearDensity blends along X first, then Y, then Z. Within a slice X is
// fixed, so the X blends of every corner row are computed once, one lane per Z
// cell; each row then performs the Y step once per cell and the Z step once per
// block. The operations, operands, and their order are exactly
// TrilinearDensity's, and SSE2 rounds each lane as scalar code does (nothing is
// contracted into an FMA), so the result is bit-identical to
// SampleTerrainDensity (the worldgen self-test checks it).
static void InterpolateSliceDensity(const CubeCorners *Corners, int LocalX,
                                    float Out[PADDED_ROWS][CHUNK_SIZE]) {
  int CellX = LocalX / DENSITY_CELL_XZ;
  __m128 Fx = _mm_set1_ps((float)(LocalX % DENSITY_CELL_XZ) /
                          (float)DENSITY_CELL_XZ);
  float FzLanes[SIMD_LANES];
  for (int Lane = 0; Lane < SIMD_LANES; Lane++) {
    FzLanes[Lane] = (float)Lane / (float)DENSITY_CELL_XZ;
  }
  __m128 Fz = _mm_loadu_ps(FzLanes);

  __m128 NearZ[DENSITY_CORNERS_Y];
  __m128 FarZ[DENSITY_CORNERS_Y];
  for (int Row = 0; Row < DENSITY_CORNERS_Y; Row++) {
    const float *Low = Corners->Values[CellX][Row];
    const float *High = Corners->Values[CellX + 1][Row];
    __m128 C0 = _mm_loadu_ps(Low);
    __m128 C1 = _mm_loadu_ps(High);
    __m128 C4 = _mm_loadu_ps(Low + 1);
    __m128 C5 = _mm_loadu_ps(High + 1);
    NearZ[Row] = _mm_add_ps(C0, _mm_mul_ps(_mm_sub_ps(C1, C0), Fx));
    FarZ[Row] = _mm_add_ps(C4, _mm_mul_ps(_mm_sub_ps(C5, C4), Fx));
  }

  for (int LocalY = 0; LocalY < PADDED_ROWS; LocalY++) {
    int CellY = LocalY / DENSITY_CELL_Y;
    __m128 Fy = _mm_set1_ps((float)(LocalY % DENSITY_CELL_Y) /
                            (float)DENSITY_CELL_Y);
    __m128 Y0 = _mm_add_ps(
        NearZ[CellY],
        _mm_mul_ps(_mm_sub_ps(NearZ[CellY + 1], NearZ[CellY]), Fy));
    __m128 Y1 = _mm_add_ps(
        FarZ[CellY], _mm_mul_ps(_mm_sub_ps(FarZ[CellY + 1], FarZ[CellY]), Fy));
    _Alignas(__m128) float Start[SIMD_LANES];
    _Alignas(__m128) float Span[SIMD_LANES];
    _mm_store_ps(Start, Y0);
    _mm_store_ps(Span, _mm_sub_ps(Y1, Y0));
    for (int Cell = 0; Cell < CELLS_PER_SLICE; Cell++) {
      __m128 Blend = _mm_add_ps(_mm_set1_ps(Start[Cell]),
                                _mm_mul_ps(_mm_set1_ps(Span[Cell]), Fz));
      _mm_store_ps(&Out[LocalY][(size_t)Cell * DENSITY_CELL_XZ], Blend);
    }
  }
}

// Open rows in the flood zone become barrier rock near the flood threshold and
// are flagged flooded above it. Flooded must arrive all false.
//
// The flood value is the trilinear blend of the cube's flood lattice, written
// out like InterpolateSliceDensity: the X blends of each lattice row are done
// once per column, then each row performs the Y and Z steps. Operations and
// their order are TrilinearDensity's, so the probe gets the same float.
static void ApplyFloodBarrier(const SliceFill *Fill, int LocalZ,
                              ColumnSlice *Slice) {
  int Lowest = 0;
  int Highest = 0;
  if (!CaveFloodRows(Fill->Column->Surface[Fill->LocalX][LocalZ], &Lowest,
                     &Highest)) {
    return;
  }
  int BaseY = Fill->BaseY;
  int First = (Lowest > BaseY) ? Lowest - BaseY : 0;
  int Last = (Highest < BaseY + PADDED_ROWS - 1) ? Highest - BaseY
                                                  : PADDED_ROWS - 1;
  bool Blended = false;
  float NearZ[3];
  float FarZ[3];
  float Threshold = GetCaveParams()->FloodThreshold;
  for (int Row = First; Row <= Last; Row++) {
    if (Slice->Density[Row][LocalZ] > 0.0F) {
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
    float Fz = (float)LocalZ / (float)CAVE_FLOOD_CELL;
    float Y0 = NearZ[CellY] + ((NearZ[CellY + 1] - NearZ[CellY]) * Fy);
    float Y1 = FarZ[CellY] + ((FarZ[CellY + 1] - FarZ[CellY]) * Fy);
    float Flood = Y0 + ((Y1 - Y0) * Fz);
    float Barrier = CaveBarrierDensity(Flood);
    if (Barrier > 0.0F) {
      Slice->Density[Row][LocalZ] = Barrier;
    } else {
      Slice->Flooded[Row][LocalZ] = Flood > Threshold;
    }
  }
}

// Solid depth of every row of an X slice, padding included, one vector of block
// columns at a time. The row above the last padding row is unknown and treated
// as non-solid; that can only shorten a depth to 8 or more, which every biome
// already renders as stone.
//
// Per lane, depth = solid ? (solid above ? depth above + 1 : 0) : -1, computed
// without branches as ((depth above + 1) & solid above) | ~solid.
static void ComputeSliceDepths(const SliceFill *Fill, ColumnSlice *Slice) {
  const __m128 ZERO = _mm_setzero_ps();
  const __m128i ONE = _mm_set1_epi32(1);
  const __m128i ALL_SET = _mm_set1_epi32(-1);
  for (int Group = 0; Group < CHUNK_SIZE; Group += SIMD_LANES) {
    __m128i AboveSolid = _mm_setzero_si128();
    __m128i AboveDepth = _mm_setzero_si128();
    for (int Row = PADDED_ROWS - 1; Row >= 0; Row--) {
      __m128i Solid = _mm_setzero_si128();
      if (Fill->BaseY + Row < Fill->Ceiling) {
        Solid = _mm_castps_si128(
            _mm_cmpgt_ps(_mm_load_ps(&Slice->Density[Row][Group]), ZERO));
      }
      __m128i Deeper = _mm_and_si128(_mm_add_epi32(AboveDepth, ONE), AboveSolid);
      __m128i Depth = _mm_or_si128(Deeper, _mm_andnot_si128(Solid, ALL_SET));
      _mm_store_si128((__m128i *)&Slice->Depths[Row][Group], Depth);
      AboveSolid = Solid;
      AboveDepth = Depth;
    }
  }
}

// Density, flooding, and solid depth of every block column of an X slice. The
// debug density, when requested, takes the cube's rows after the flood barrier.
static void ComputeSliceColumns(const SliceFill *Fill, ColumnSlice *Slice,
                                float *DebugDensity) {
  InterpolateSliceDensity(Fill->Corners, Fill->LocalX, Slice->Density);
  memset(Slice->Flooded, 0, sizeof(Slice->Flooded));
  for (int LocalZ = 0; LocalZ < CHUNK_SIZE; LocalZ++) {
    ApplyFloodBarrier(Fill, LocalZ, Slice);
  }
  if (DebugDensity != NULL) {
    // DebugDensity is [X][Y][Z], so the slice's first CHUNK_SIZE rows are one
    // contiguous block of it.
    memcpy(&DebugDensity[(size_t)Fill->LocalX * CHUNK_SIZE * CHUNK_SIZE],
           Slice->Density, sizeof(float) * CHUNK_SIZE * CHUNK_SIZE);
  }
  ComputeSliceDepths(Fill, Slice);
}

// Whether every row of a slice's block column is open (non-solid) and
// unflooded: then each block takes the fluid its height alone decides.
static bool ColumnIsOpenFluid(const ColumnSlice *Slice, int LocalZ) {
  for (int Row = 0; Row < CHUNK_SIZE; Row++) {
    if (Slice->Depths[Row][LocalZ] >= 0 || Slice->Flooded[Row][LocalZ]) {
      return false;
    }
  }
  return true;
}

// An open, unflooded overworld block column, filled by fluid range instead of
// block by block: the blocks DeterminePaletteBlock gives it, CaveFluidAt's
// fluid and the frozen sea surface (FrozenSea: the biome at the sea level row
// freezes). Returns how many blocks are not air.
static int FillFluidColumn(Chunk *ChunkVal, int LocalX, int LocalZ, int BaseY,
                           int Surface, bool FrozenSea) {
  const TerrainBaseBlocks *Base = GetTerrainBaseBlocks();
  CaveFluidRows Rows = CaveUnfloodedFluidRows(Surface);
  int SolidCount = 0;
  for (int LocalY = 0; LocalY < CHUNK_SIZE; LocalY++) {
    int GlobalY = BaseY + LocalY;
    BlockID BlockId = BLOCK_AIR;
    if (GlobalY >= Rows.WaterLow && GlobalY <= Rows.WaterHigh) {
      BlockId = (FrozenSea && GlobalY == SEA_LEVEL) ? Base->IceId
                                                    : Base->WaterId;
    } else if (GlobalY <= Rows.LavaHigh) {
      BlockId = Base->LavaId;
    }
    (*ChunkVal->Blocks)[LocalX][LocalY][LocalZ] = BlockId;
    if (BlockId != BLOCK_AIR) {
      SolidCount++;
    }
  }
  return SolidCount;
}

// Corners is NULL when the cube has no overworld layer. FluidColumns lets open
// columns skip the per-block decision; without it every block takes the full
// path, which is what the self-test compares the shortcut against.
static int FillCubePerBlock(Chunk *ChunkVal, const ColumnEntry *Column,
                            const CubeCorners *Corners,
                            const StratumGenerator *Layers, uint64_t SeedVal,
                            float *DebugDensity, bool FluidColumns) {
  int SolidCount = 0;
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  int Ceiling = GetGenerationCeiling();
  CubeFlood Flood = {.Loaded = false};
  // Built on the first block deeper than the palette layers.
  FillerCube Filler;
  bool HaveFiller = false;
  ColumnSlice Slice;
  if (Corners == NULL) {
    memset(Slice.Depths, 0, sizeof(Slice.Depths));
    memset(Slice.Flooded, 0, sizeof(Slice.Flooded));
  }
  bool AllOverworld = true;
  for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
    AllOverworld &= (Layers[IdxY] == STRATUM_GEN_OVERWORLD);
  }
  // The row holding the sea surface, where frozen biomes put ice, or -1.
  int SeaRow = SEA_LEVEL - BaseY;
  if (SeaRow < 0 || SeaRow >= CHUNK_SIZE) {
    SeaRow = -1;
  }

  // One biome per BIOME_CELL_SIZE^3 cell, so it is resolved per cell rather
  // than per block.
  const BiomeType *Biomes[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK]
                         [BIOME_CELLS_PER_CHUNK];
  for (int CellX = 0; CellX < BIOME_CELLS_PER_CHUNK; CellX++) {
    for (int CellY = 0; CellY < BIOME_CELLS_PER_CHUNK; CellY++) {
      for (int CellZ = 0; CellZ < BIOME_CELLS_PER_CHUNK; CellZ++) {
        Biomes[CellX][CellY][CellZ] =
            GetBiomeDef(ChunkVal->BiomeMap[CellX][CellY][CellZ]);
      }
    }
  }

  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    if (Corners != NULL) {
      SliceFill Fill = {.Corners = Corners,
                        .Flood = &Flood,
                        .ChunkVal = ChunkVal,
                        .Column = Column,
                        .SeedVal = SeedVal,
                        .LocalX = IdxX,
                        .BaseY = BaseY,
                        .Ceiling = Ceiling};
      ComputeSliceColumns(&Fill, &Slice, DebugDensity);
    }

    for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
      // Most open columns (sky over land, the sea above its floor) need no
      // per-block decision at all.
      if (FluidColumns && Corners != NULL && AllOverworld &&
          ColumnIsOpenFluid(&Slice, IdxZ)) {
        bool FrozenSea =
            SeaRow >= 0 && Biomes[IdxX / BIOME_CELL_SIZE]
                                 [SeaRow / BIOME_CELL_SIZE]
                                 [IdxZ / BIOME_CELL_SIZE]->FrozenWater;
        SolidCount += FillFluidColumn(ChunkVal, IdxX, IdxZ, BaseY,
                                      Column->Surface[IdxX][IdxZ], FrozenSea);
        continue;
      }
      int GlobalX = (ChunkVal->ChunkX * CHUNK_SIZE) + IdxX;
      int GlobalZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + IdxZ;
      // Built on the column's first filler block; rows outside it take the
      // row's layer block directly.
      uint32_t SlowRows = 0;
      bool HaveSlowRows = false;

      for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
        int GlobalY = BaseY + IdxY;
        BlockID BlockId = BLOCK_AIR;

        switch (Layers[IdxY]) {
        case STRATUM_GEN_OVERWORLD: {
          const BiomeType *Biome =
              Biomes[IdxX / BIOME_CELL_SIZE][IdxY / BIOME_CELL_SIZE]
                    [IdxZ / BIOME_CELL_SIZE];
          int Depth = Slice.Depths[IdxY][IdxZ];
          if (Depth >= Biome->SubsurfaceDepth) {
            if (!HaveFiller) {
              FillerCubeInit(&Filler, ChunkVal->ChunkX, ChunkVal->ChunkY,
                             ChunkVal->ChunkZ, SeedVal);
              HaveFiller = true;
            }
            if (!HaveSlowRows) {
              SlowRows = FillerColumnSlowRows(&Filler, GlobalX, GlobalZ);
              HaveSlowRows = true;
            }
            BlockId = (((SlowRows >> (unsigned)IdxY) & 1U) != 0U)
                          ? FillerBlockAt(&Filler, GlobalX, GlobalY, GlobalZ)
                          : Filler.RowBlock[IdxY];
            break;
          }
          bool Steep = Corners != NULL && Biome->SteepBlockId >= 0 &&
                       Depth >= 0 && Depth < Biome->SubsurfaceDepth &&
                       IsSteepAt(Corners, IdxX, IdxY, IdxZ);
          BlockId = DeterminePaletteBlock(Biome, Depth, GlobalY,
                                          Column->Surface[IdxX][IdxZ],
                                          Slice.Flooded[IdxY][IdxZ], Steep);
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
                                             .DebugDensity = NULL,
                                             .CornerRows = NULL};
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
  int ReusedPlanes = 0;
  CubeClass Class = CUBE_CLASS_MIXED;
  if (AllVoid) {
    Class = CUBE_CLASS_AIR;
  } else if (AllOverworld) {
    Class = ClassifyByBounds(BaseY, Column, Ceiling);
    if (Class == CUBE_CLASS_MIXED) {
      ReusedPlanes = EvaluateCubeCorners(ChunkVal, Column, SeedVal,
                                         Options->CornerRows, &Corners);
      HaveCorners = true;
      Class = ClassifyByCorners(BaseY, Column, &Corners, Ceiling);
    }
  }

  ClearDebugDensity(Options->DebugDensity);
  int SolidCount = 0;
  if (!Options->AllowClassification || Class == CUBE_CLASS_MIXED) {
    if (AnyOverworld && !HaveCorners) {
      ReusedPlanes = EvaluateCubeCorners(ChunkVal, Column, SeedVal,
                                         Options->CornerRows, &Corners);
      HaveCorners = true;
    }
    SolidCount = ChunkBeginFill(ChunkVal)
                     ? FillCubePerBlock(ChunkVal, Column,
                                        HaveCorners ? &Corners : NULL, Layers,
                                        SeedVal, Options->DebugDensity,
                                        Options->AllowClassification)
                     : -1;
  } else if (Class == CUBE_CLASS_AIR) {
    // Sky and void cubes: no noise sampled, no cell written, no buffer leased.
    SolidCount = FillCubeUniform(ChunkVal, BLOCK_AIR);
  } else if (Class == CUBE_CLASS_WATER) {
    SolidCount = FillCubeUniform(ChunkVal, GetTerrainBaseBlocks()->WaterId);
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
    if (HaveCorners) {
      atomic_uint_fast64_t *Planes = GetCornerPlaneCounters();
      atomic_fetch_add_explicit(&Planes[0], (uint_fast64_t)ReusedPlanes,
                                memory_order_relaxed);
      atomic_fetch_add_explicit(
          &Planes[1], (uint_fast64_t)(DENSITY_CORNERS_Y - ReusedPlanes),
          memory_order_relaxed);
    }
  }

  ChunkVal->SolidBlockCount = SolidCount;
  ChunkVal->IsDirty = true;
  return Class;
}

void ChunkAttachBuffer(Chunk *ChunkVal, DenseBlockBuffer *Buffer) {
  ChunkVal->Pool = (struct DenseBlockPool *)0;
  ChunkVal->Blocks = Buffer;
  ChunkVal->Packed = (PackedBlockBuffer *)0;
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
  if (ChunkVal->Pool != (struct DenseBlockPool *)0) {
    DensePoolRelease(ChunkVal->Pool, ChunkVal->Blocks);
    PackedPoolRelease(ChunkVal->Pool, ChunkVal->Packed);
  }
  ChunkVal->Blocks = (DenseBlockBuffer *)0;
  ChunkVal->Packed = (PackedBlockBuffer *)0;
  ChunkVal->UniformBlockId = BlockId;
  ChunkVal->IsUniform = true;
}

// Slot of Id in the palette, appending it when absent. -1 when the palette is
// full and Id is not in it.
static int PackedPaletteSlot(PackedBlockBuffer *Packed, BlockID Id) {
  for (int Slot = 0; Slot < Packed->PaletteCount; Slot++) {
    if (Packed->Palette[Slot] == Id) {
      return Slot;
    }
  }
  if (Packed->PaletteCount >= PACKED_PALETTE_CAPACITY) {
    return -1;
  }
  Packed->Palette[Packed->PaletteCount] = Id;
  return Packed->PaletteCount++;
}

// Flat cell order is [x][y][z] with z innermost, so a cell's parity is its Z
// parity and the flat byte index is the cell index halved.
static void PackedWriteFlat(PackedBlockBuffer *Packed, size_t Cell, int Slot) {
  uint8_t *Bytes = &Packed->Indices[0][0][0];
  unsigned Shift = (unsigned)(Cell & 1U) * PACKED_INDEX_BITS;
  unsigned Keep = ~((unsigned)PACKED_INDEX_MASK << Shift);
  Bytes[Cell >> 1] =
      (uint8_t)(((unsigned)Bytes[Cell >> 1] & Keep) | ((unsigned)Slot << Shift));
}

// Packs dense cells into Out. False when they hold more ids than a palette can.
static bool PackCells(const BlockID *Cells, PackedBlockBuffer *Out) {
  memset(Out, 0, sizeof(*Out));
  BlockID LastId = Cells[0];
  int LastSlot = PackedPaletteSlot(Out, LastId);
  for (size_t Cell = 0; Cell < CHUNK_VOLUME; Cell++) {
    if (Cells[Cell] != LastId) {
      LastId = Cells[Cell];
      LastSlot = PackedPaletteSlot(Out, LastId);
      if (LastSlot < 0) {
        return false;
      }
    }
    PackedWriteFlat(Out, Cell, LastSlot);
  }
  return true;
}

static void UnpackCells(const PackedBlockBuffer *Packed, BlockID *Cells) {
  const uint8_t *Bytes = &Packed->Indices[0][0][0];
  for (size_t Byte = 0; Byte < CHUNK_VOLUME / PACKED_CELLS_PER_BYTE; Byte++) {
    unsigned Value = Bytes[Byte];
    Cells[Byte * 2] = Packed->Palette[Value & PACKED_INDEX_MASK];
    Cells[(Byte * 2) + 1] = Packed->Palette[Value >> PACKED_INDEX_BITS];
  }
}

bool ChunkBeginFill(Chunk *ChunkVal) {
  if (ChunkVal->Packed != (PackedBlockBuffer *)0) {
    return PromoteChunkToDense(ChunkVal);
  }
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

// A chunk whose packed buffer cannot be leased simply stays dense.
void ChunkEndFill(Chunk *ChunkVal) {
  if (ChunkVal->Blocks == (DenseBlockBuffer *)0 ||
      ChunkVal->Pool == (struct DenseBlockPool *)0) {
    return;
  }
  PackedBlockBuffer Scratch;
  if (!PackCells(&(*ChunkVal->Blocks)[0][0][0], &Scratch)) {
    return;
  }
  if (Scratch.PaletteCount == 1) {
    ChunkResetStorage(ChunkVal, Scratch.Palette[0]);
    return;
  }
  PackedBlockBuffer *Packed = PackedPoolAcquire(ChunkVal->Pool);
  if (Packed == (PackedBlockBuffer *)0) {
    return;
  }
  *Packed = Scratch;
  DensePoolRelease(ChunkVal->Pool, ChunkVal->Blocks);
  ChunkVal->Blocks = (DenseBlockBuffer *)0;
  ChunkVal->Packed = Packed;
  ChunkVal->IsUniform = false;
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
  if (ChunkVal->Packed != (PackedBlockBuffer *)0) {
    UnpackCells(ChunkVal->Packed, Cells);
    PackedPoolRelease(ChunkVal->Pool, ChunkVal->Packed);
    ChunkVal->Packed = (PackedBlockBuffer *)0;
  } else {
    for (size_t Idx = 0; Idx < CHUNK_VOLUME; Idx++) {
      Cells[Idx] = ChunkVal->UniformBlockId;
    }
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
      // A packed chunk stays packed while its palette can take the id; only
      // the edit that would need a seventeenth id promotes it.
      int Slot = (ChunkVal->Packed != (PackedBlockBuffer *)0)
                     ? PackedPaletteSlot(ChunkVal->Packed, BlockId)
                     : -1;
      if (Slot < 0 && !PromoteChunkToDense(ChunkVal)) {
        LogError("CHUNK: no block buffer to promote (%d, %d, %d); edit dropped",
                 ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
        return;
      }
      if (OldId == 0 && BlockId != 0) {
        ChunkVal->SolidBlockCount++;
      } else if (OldId != 0 && BlockId == 0) {
        ChunkVal->SolidBlockCount--;
      }

      if (Slot >= 0) {
        size_t Cell = ((((size_t)IdxX * CHUNK_SIZE) + (size_t)IdxY) * CHUNK_SIZE) +
                      (size_t)IdxZ;
        PackedWriteFlat(ChunkVal->Packed, Cell, Slot);
      } else {
        (*ChunkVal->Blocks)[IdxX][IdxY][IdxZ] = BlockId;
      }
      ChunkVal->IsDirty = true;
      ChunkVal->IsModified = true;
    }
  }
}
