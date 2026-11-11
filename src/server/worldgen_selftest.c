#include "server/worldgen_selftest.h"
#include "core/log.h"
#include "core/noise.h"
#include "persistence/world_save.h"
#include "world/chunk.h"
#include "world/feature.h"
#include "world/block_system.h"
#include "world/stone_filler.h"
#include "world/surface.h"
#include "world/terrain_density.h"
#include "world/terrain_shape.h"
#include "world/worldgen_column_cache.h"
#include "world/worldgen_strata.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
  // A 4x4 block of columns around the origin plus one far column, so surface
  // cubes cover both land and sea and the far column exercises a different
  // stretch of every noise field.
  TEST_COLUMN_MIN = -2,
  TEST_COLUMN_MAX = 1,
  FAR_COLUMN = 31,
  TEST_COLUMN_COUNT = ((TEST_COLUMN_MAX - TEST_COLUMN_MIN + 1) *
                       (TEST_COLUMN_MAX - TEST_COLUMN_MIN + 1)) + 1
};

// Cube layers chosen to hit every path: above the ceiling and just below it,
// around the surface, deep stone, the bedrock band and its overworld neighbour,
// and the reserved void below the floor.
static const int TEST_CHUNK_YS[] = {40, 39, 2, 1, 0, -1, -2, -10, -32, -33, -34};
static const char *const CLASS_NAMES[CUBE_CLASS_COUNT] = {"AIR", "SOLID",
                                                          "MIXED"};

typedef struct {
  Chunk *Reference;
  Chunk *PerBlock;
  Chunk *Uncached;
  ColumnEntry *Fresh;
  // Interpolated density of the per-block run, compared against the probe.
  float *Density;
  int ClassCounts[CUBE_CLASS_COUNT];
  int ProbedBlocks;
  int NoiseSamples;
  float MaxNoise;
  int FluidProbes;
  int FloodedWater;
  int BiomeCells;
} SelfTestScratch;

enum {
  // Every Nth block of a MIXED cube is probed: a fixed, spread-out subset that
  // keeps the check fast while touching every cell of the cube.
  PROBE_STRIDE = 7
};

static void GetTestColumn(int Index, int *ColumnX, int *ColumnZ) {
  const int SIDE = TEST_COLUMN_MAX - TEST_COLUMN_MIN + 1;
  if (Index == TEST_COLUMN_COUNT - 1) {
    *ColumnX = FAR_COLUMN;
    *ColumnZ = FAR_COLUMN;
    return;
  }
  *ColumnX = TEST_COLUMN_MIN + (Index % SIDE);
  *ColumnZ = TEST_COLUMN_MIN + (Index / SIDE);
}

// Climate values are compared exactly: the cache must store what the pure
// sampler returned, not something close to it.
static bool ClimateEqual(const ColumnEntry *Left, const ColumnEntry *Right) {
  for (int CellX = 0; CellX < BIOME_CELLS_PER_CHUNK; CellX++) {
    for (int CellZ = 0; CellZ < BIOME_CELLS_PER_CHUNK; CellZ++) {
      const BiomeClimate *A = &Left->Climate[CellX][CellZ];
      const BiomeClimate *B = &Right->Climate[CellX][CellZ];
      for (int Axis = 0; Axis < CLIMATE_AXIS_COUNT; Axis++) {
        if (A->Axes[Axis] != B->Axes[Axis]) {
          return false;
        }
      }
      if (A->Depth != B->Depth ||
          Left->SurfaceBiome[CellX][CellZ] != Right->SurfaceBiome[CellX][CellZ] ||
          Left->CellFloor[CellX][CellZ] != Right->CellFloor[CellX][CellZ]) {
        return false;
      }
    }
  }
  return true;
}

static bool ShapeCornersEqual(const ColumnEntry *Left,
                              const ColumnEntry *Right) {
  for (int CornerX = 0; CornerX < COLUMN_SHAPE_CORNERS; CornerX++) {
    for (int CornerZ = 0; CornerZ < COLUMN_SHAPE_CORNERS; CornerZ++) {
      if (Left->CornerBase[CornerX][CornerZ] !=
              Right->CornerBase[CornerX][CornerZ] ||
          Left->CornerShell[CornerX][CornerZ] !=
              Right->CornerShell[CornerX][CornerZ] ||
          Left->CornerVariation[CornerX][CornerZ] !=
              Right->CornerVariation[CornerX][CornerZ]) {
        return false;
      }
    }
  }
  return Left->BoundHigh == Right->BoundHigh &&
         Left->BoundLow == Right->BoundLow;
}

// Every cached block-column surface must equal the pure function, which blends
// the same corners through its own lookups.
static bool SurfacesMatchPure(const ColumnEntry *Entry, uint64_t Seed) {
  for (int LocalX = 0; LocalX < CHUNK_SIZE; LocalX++) {
    for (int LocalZ = 0; LocalZ < CHUNK_SIZE; LocalZ++) {
      int Pure = GetPreliminarySurface((Entry->ColumnX * CHUNK_SIZE) + LocalX,
                                       (Entry->ColumnZ * CHUNK_SIZE) + LocalZ,
                                       Seed);
      if (Entry->Surface[LocalX][LocalZ] != Pure) {
        return false;
      }
    }
  }
  for (int CellX = 0; CellX < BIOME_CELLS_PER_CHUNK; CellX++) {
    for (int CellZ = 0; CellZ < BIOME_CELLS_PER_CHUNK; CellZ++) {
      int Center = BIOME_CELL_SIZE / 2;
      int Floor = GetPreliminaryFloor(
          (Entry->ColumnX * CHUNK_SIZE) + (CellX * BIOME_CELL_SIZE) + Center,
          (Entry->ColumnZ * CHUNK_SIZE) + (CellZ * BIOME_CELL_SIZE) + Center,
          Seed);
      if (Entry->CellFloor[CellX][CellZ] != Floor) {
        return false;
      }
    }
  }
  return true;
}

static bool EntriesEqual(const ColumnEntry *Left, const ColumnEntry *Right) {
  return ShapeCornersEqual(Left, Right) && Left->ColumnX == Right->ColumnX &&
         Left->ColumnZ == Right->ColumnZ &&
         Left->Seed == Right->Seed && Left->MinSurface == Right->MinSurface &&
         Left->MaxSurface == Right->MaxSurface &&
         memcmp(Left->Surface, Right->Surface, sizeof(Left->Surface)) == 0 &&
         ClimateEqual(Left, Right);
}

static bool ChunksMatch(const Chunk *Left, const Chunk *Right) {
  return Left->SolidBlockCount == Right->SolidBlockCount &&
         memcmp(Left->Data, Right->Data, sizeof(Left->Data)) == 0 &&
         memcmp(Left->BiomeMap, Right->BiomeMap, sizeof(Left->BiomeMap)) == 0;
}

// The same steps a chunk worker runs on a freshly generated (not loaded) cube.
// Accelerated runs (Classify) also let the structure pass skip unreachable
// cubes; the per-block run disables both, so comparing them covers the skip.
static CubeClass GenerateCube(Chunk *Out, int ChunkX, int ChunkY, int ChunkZ,
                              const ColumnEntry *Column, bool Classify,
                              float *Density) {
  Out->ChunkX = ChunkX;
  Out->ChunkY = ChunkY;
  Out->ChunkZ = ChunkZ;

  // A private copy: the structure pass looks up neighbouring columns in the
  // main-thread cache, which may reuse the slot Column points into.
  static ColumnEntry SLocal;
  SLocal = *Column;

  const TerrainGenOptions OPTIONS = {.AllowClassification = Classify,
                                     .CountClasses = false,
                                     .DebugDensity = Density};
  FillChunkBiomeMap(Out, &SLocal);
  CubeClass Class = GenerateChunkTerrainEx(Out, &SLocal, &OPTIONS);
  PlaceChunkFeatures(Out, GetColumnCache(COLUMN_CACHE_MAIN_SLOT), Classify);
  PlaceChunkFlora(Out, &SLocal);
  return Class;
}

// The pure probe must return exactly the float the generator interpolated.
static bool CheckProbe(SelfTestScratch *Scratch, int ChunkX, int ChunkY,
                       int ChunkZ, uint64_t Seed) {
  for (int Index = 0; Index < (int)CHUNK_VOLUME; Index += PROBE_STRIDE) {
    float Generated = Scratch->Density[Index];
    if (isnan(Generated)) {
      continue;
    }
    int LocalX = Index / (CHUNK_SIZE * CHUNK_SIZE);
    int LocalY = (Index / CHUNK_SIZE) % CHUNK_SIZE;
    int LocalZ = Index % CHUNK_SIZE;
    int X = (ChunkX * CHUNK_SIZE) + LocalX;
    int Y = (ChunkY * CHUNK_SIZE) + LocalY;
    int Z = (ChunkZ * CHUNK_SIZE) + LocalZ;
    float Probed = SampleTerrainDensity(X, Y, Z, Seed);
    if (Probed != Generated) {
      LogError("WORLDGEN_TEST: FAIL probe at (%d, %d, %d) gives %.9g, the "
               "generator interpolated %.9g",
               X, Y, Z, (double)Probed, (double)Generated);
      return false;
    }
    Scratch->ProbedBlocks++;
  }
  return true;
}

// Fluids of the classified run against the probe, and flooded cave water never
// beside dry air. A block a feature replaced is no longer a fluid and is
// skipped; so is anything outside the overworld layer.
static bool CheckFluids(SelfTestScratch *Scratch, int ChunkX, int ChunkY,
                        int ChunkZ, const ColumnEntry *Column) {
  const Chunk *Cube = Scratch->Reference;
  BlockID Water = TerrainFluidBlock(CAVE_FLUID_WATER);
  BlockID Lava = TerrainFluidBlock(CAVE_FLUID_LAVA);
  const int STEP_X[4] = {1, -1, 0, 0};
  const int STEP_Z[4] = {0, 0, 1, -1};
  for (int Index = 0; Index < (int)CHUNK_VOLUME; Index++) {
    int LocalX = Index / (CHUNK_SIZE * CHUNK_SIZE);
    int LocalY = (Index / CHUNK_SIZE) % CHUNK_SIZE;
    int LocalZ = Index % CHUNK_SIZE;
    int Y = (ChunkY * CHUNK_SIZE) + LocalY;
    BlockID Block = Cube->Data[LocalX][LocalY][LocalZ];
    bool IsFluid = Block == 0 || Block == Water || Block == Lava;
    if (!IsFluid || ResolveStratum(Y) != STRATUM_GEN_OVERWORLD ||
        Y >= GetGenerationCeiling() || !(Scratch->Density[Index] <= 0.0F)) {
      continue;
    }
    int X = (ChunkX * CHUNK_SIZE) + LocalX;
    int Z = (ChunkZ * CHUNK_SIZE) + LocalZ;
    if (Index % PROBE_STRIDE == 0) {
      BlockID Expected =
          TerrainFluidBlock((int)SampleTerrainFluid(X, Y, Z, Column->Seed));
      if (Expected != Block) {
        LogError("WORLDGEN_TEST: FAIL fluid at (%d, %d, %d) is block %d, the "
                 "probe gives %d",
                 X, Y, Z, (int)Block, (int)Expected);
        return false;
      }
      Scratch->FluidProbes++;
    }
    if (Block != Water ||
        !CaveFloodZone(Y, Column->Surface[LocalX][LocalZ])) {
      continue;
    }
    Scratch->FloodedWater++;
    for (int Side = 0; Side < 4; Side++) {
      int NearX = LocalX + STEP_X[Side];
      int NearZ = LocalZ + STEP_Z[Side];
      if (NearX < 0 || NearX >= CHUNK_SIZE || NearZ < 0 ||
          NearZ >= CHUNK_SIZE) {
        continue;
      }
      if (Cube->Data[NearX][LocalY][NearZ] == 0) {
        LogError("WORLDGEN_TEST: FAIL flooded water at (%d, %d, %d) borders "
                 "dry air",
                 X, Y, Z);
        return false;
      }
    }
  }
  return true;
}

// Every biome map cell must be what the pure sampler gives anywhere in that
// cell, whether the generator copied the column's cached surface biome or
// resolved the cell itself.
static bool CheckBiomeMap(SelfTestScratch *Scratch, int ChunkX, int ChunkY,
                          int ChunkZ, uint64_t Seed) {
  for (int CellX = 0; CellX < BIOME_CELLS_PER_CHUNK; CellX++) {
    for (int CellY = 0; CellY < BIOME_CELLS_PER_CHUNK; CellY++) {
      for (int CellZ = 0; CellZ < BIOME_CELLS_PER_CHUNK; CellZ++) {
        int X = (ChunkX * CHUNK_SIZE) + (CellX * BIOME_CELL_SIZE);
        int Y = (ChunkY * CHUNK_SIZE) + (CellY * BIOME_CELL_SIZE) +
                (BIOME_CELL_SIZE - 1);
        int Z = (ChunkZ * CHUNK_SIZE) + (CellZ * BIOME_CELL_SIZE) + 1;
        int Pure = SampleBiomeAt(X, Y, Z, Seed, NULL);
        int Stored = Scratch->Reference->BiomeMap[CellX][CellY][CellZ];
        if (Pure != Stored) {
          LogError("WORLDGEN_TEST: FAIL biome cell at (%d, %d, %d) holds %d, "
                   "the pure sampler gives %d",
                   X, Y, Z, Stored, Pure);
          return false;
        }
        Scratch->BiomeCells++;
      }
    }
  }
  return true;
}

// The bound tier of classification assumes |n3| <= TERRAIN_NOISE_BOUND; sample
// the 3D field at every grid corner of the cube to back that up.
static bool CheckNoiseBound(SelfTestScratch *Scratch, int ChunkX, int ChunkY,
                            int ChunkZ, uint64_t Seed) {
  for (int CornerX = 0; CornerX < DENSITY_CORNERS_XZ; CornerX++) {
    for (int CornerY = 0; CornerY < DENSITY_CORNERS_Y; CornerY++) {
      for (int CornerZ = 0; CornerZ < DENSITY_CORNERS_XZ; CornerZ++) {
        int X = (ChunkX * CHUNK_SIZE) + (CornerX * DENSITY_CELL_XZ);
        int Y = (ChunkY * CHUNK_SIZE) + (CornerY * DENSITY_CELL_Y);
        int Z = (ChunkZ * CHUNK_SIZE) + (CornerZ * DENSITY_CELL_XZ);
        float Noise = fabsf(SampleTerrainNoise3D(X, Y, Z, Seed));
        Scratch->NoiseSamples++;
        Scratch->MaxNoise = fmaxf(Scratch->MaxNoise, Noise);
        if (Noise > TERRAIN_NOISE_BOUND) {
          LogError("WORLDGEN_TEST: FAIL 3D terrain noise %.4f at (%d, %d, %d) "
                   "exceeds the classification bound %.2f",
                   (double)Noise, X, Y, Z, (double)TERRAIN_NOISE_BOUND);
          return false;
        }
      }
    }
  }
  return true;
}

static bool CheckCube(SelfTestScratch *Scratch, int ChunkX, int ChunkY,
                      int ChunkZ, const ColumnEntry *CachedEntry) {
  // Copied before the first run: its structure pass looks up neighbouring
  // columns, which may reuse the cache slot CachedEntry points into.
  static ColumnEntry SCached;
  SCached = *CachedEntry;
  const ColumnEntry *Cached = &SCached;

  CubeClass Class = GenerateCube(Scratch->Reference, ChunkX, ChunkY, ChunkZ,
                                 Cached, true, NULL);
  Scratch->ClassCounts[Class]++;
  if (!CheckBiomeMap(Scratch, ChunkX, ChunkY, ChunkZ, Cached->Seed)) {
    return false;
  }

  (void)GenerateCube(Scratch->PerBlock, ChunkX, ChunkY, ChunkZ, Cached, false,
                     Scratch->Density);
  if (!ChunksMatch(Scratch->Reference, Scratch->PerBlock)) {
    LogError("WORLDGEN_TEST: FAIL cube (%d, %d, %d) classified %s differs from "
             "the per-block path",
             ChunkX, ChunkY, ChunkZ, CLASS_NAMES[Class]);
    return false;
  }

  if (Class == CUBE_CLASS_MIXED &&
      (!CheckProbe(Scratch, ChunkX, ChunkY, ChunkZ, Cached->Seed) ||
       !CheckNoiseBound(Scratch, ChunkX, ChunkY, ChunkZ, Cached->Seed) ||
       !CheckFluids(Scratch, ChunkX, ChunkY, ChunkZ, Cached))) {
    return false;
  }

  (void)GenerateCube(Scratch->Uncached, ChunkX, ChunkY, ChunkZ, Scratch->Fresh,
                     true, NULL);
  if (!ChunksMatch(Scratch->Reference, Scratch->Uncached)) {
    LogError("WORLDGEN_TEST: FAIL cube (%d, %d, %d) differs when its column "
             "entry is not cached",
             ChunkX, ChunkY, ChunkZ);
    return false;
  }
  return true;
}

static bool CheckColumn(SelfTestScratch *Scratch, int ColumnX, int ColumnZ,
                        uint64_t Seed) {
  const ColumnEntry *Cached = ColumnCacheLookup(
      GetColumnCache(COLUMN_CACHE_MAIN_SLOT), ColumnX, ColumnZ, Seed);
  ColumnEntryFill(Scratch->Fresh, ColumnX, ColumnZ, Seed);
  if (!EntriesEqual(Cached, Scratch->Fresh) ||
      !SurfacesMatchPure(Cached, Seed)) {
    LogError("WORLDGEN_TEST: FAIL column (%d, %d) cache entry differs from the "
             "pure samplers",
             ColumnX, ColumnZ);
    return false;
  }

  for (size_t Idx = 0; Idx < sizeof(TEST_CHUNK_YS) / sizeof(TEST_CHUNK_YS[0]);
       Idx++) {
    // Re-looked up per cube: a lookup must keep returning the same entry.
    Cached = ColumnCacheLookup(GetColumnCache(COLUMN_CACHE_MAIN_SLOT), ColumnX,
                               ColumnZ, Seed);
    if (!CheckCube(Scratch, ColumnX, TEST_CHUNK_YS[Idx], ColumnZ, Cached)) {
      return false;
    }
  }

  LogInfo("WORLDGEN_TEST: ok column (%d, %d): cache entry matches the pure "
          "samplers; %d cubes identical with classification off and uncached",
          ColumnX, ColumnZ,
          (int)(sizeof(TEST_CHUNK_YS) / sizeof(TEST_CHUNK_YS[0])));
  return true;
}

static bool CheckSplines(void) {
  enum { FAILURE_CHARS = 160 };
  char Failure[FAILURE_CHARS] = {0};
  int Checked = 0;
  if (!TerrainShapeSelfCheck(Failure, (int)sizeof(Failure), &Checked)) {
    LogError("WORLDGEN_TEST: FAIL terrain shape %s", Failure);
    return false;
  }
  LogInfo("WORLDGEN_TEST: ok terrain shape splines reproduce %d control points "
          "and are continuous at each",
          Checked);
  return true;
}

static int FloorDivChunk(int Value) {
  int Quotient = Value / CHUNK_SIZE;
  if ((Value % CHUNK_SIZE != 0) && (Value < 0)) {
    Quotient--;
  }
  return Quotient;
}

// The fixed test columns need not contain a structure, which would leave the
// structure pass's skip and ground probe unexercised. This aims at the first
// structure the pass really places: the cubes around its anchor go through the
// same three-way comparison, and the anchor cube must hold the structure's base.
static bool CheckStructureRegion(SelfTestScratch *Scratch, uint64_t Seed) {
  enum { SEARCH_CELLS = 64 };
  int X = 0;
  int Y = 0;
  int Z = 0;
  if (!FindStructureAnchor(SEARCH_CELLS, Seed, &X, &Y, &Z)) {
    LogError("WORLDGEN_TEST: FAIL no structure within %d grid cells of the "
             "origin, so the structure pass went unchecked",
             SEARCH_CELLS);
    return false;
  }

  int AnchorChunkX = FloorDivChunk(X);
  int AnchorChunkY = FloorDivChunk(Y);
  int AnchorChunkZ = FloorDivChunk(Z);
  for (int Dx = -1; Dx <= 1; Dx++) {
    for (int Dz = -1; Dz <= 1; Dz++) {
      for (int Dy = 0; Dy <= 1; Dy++) {
        // The uncached run reads Fresh, so it must describe this column.
        ColumnEntryFill(Scratch->Fresh, AnchorChunkX + Dx, AnchorChunkZ + Dz,
                        Seed);
        const ColumnEntry *Entry = ColumnCacheLookup(
            GetColumnCache(COLUMN_CACHE_MAIN_SLOT), AnchorChunkX + Dx,
            AnchorChunkZ + Dz, Seed);
        if (!CheckCube(Scratch, AnchorChunkX + Dx, AnchorChunkY + Dy,
                       AnchorChunkZ + Dz, Entry)) {
          return false;
        }
      }
    }
  }

  const ColumnEntry *Anchor =
      ColumnCacheLookup(GetColumnCache(COLUMN_CACHE_MAIN_SLOT), AnchorChunkX,
                        AnchorChunkZ, Seed);
  static ColumnEntry SAnchor;
  SAnchor = *Anchor;
  (void)GenerateCube(Scratch->Reference, AnchorChunkX, AnchorChunkY,
                     AnchorChunkZ, &SAnchor, true, NULL);
  BlockID Base = Scratch->Reference->Data[X - (AnchorChunkX * CHUNK_SIZE)]
                                         [Y - (AnchorChunkY * CHUNK_SIZE)]
                                         [Z - (AnchorChunkZ * CHUNK_SIZE)];
  if (Base == 0) {
    LogError("WORLDGEN_TEST: FAIL the structure anchored at (%d, %d, %d) left "
             "its base cell empty",
             X, Y, Z);
    return false;
  }

  LogInfo("WORLDGEN_TEST: ok structure at (%d, %d, %d): the 18 cubes around it "
          "are identical with the structure skip on and off",
          X, Y, Z);
  return true;
}

// The fixed test cubes need not reach a flooded cave. This walks cubes below
// the origin whose flood lattice point is well above the threshold until one
// really holds flooded water, so the fluid checks always see some.
static bool CheckFloodRegion(SelfTestScratch *Scratch, uint64_t Seed) {
  enum { SEARCH_RADIUS = 3, SEARCH_DEPTH = 28, MAX_CANDIDATES = 24 };
  const float MARGIN = 0.1F;
  float Wanted = GetCaveParams()->FloodThreshold + MARGIN;
  int Candidates = 0;
  int Before = Scratch->FloodedWater;
  for (int ChunkY = -2; ChunkY >= -SEARCH_DEPTH; ChunkY--) {
    for (int ChunkX = -SEARCH_RADIUS; ChunkX <= SEARCH_RADIUS; ChunkX++) {
      for (int ChunkZ = -SEARCH_RADIUS; ChunkZ <= SEARCH_RADIUS; ChunkZ++) {
        if (CaveFloodLattice(ChunkX * CHUNK_SIZE, ChunkY * CHUNK_SIZE,
                             ChunkZ * CHUNK_SIZE, Seed) < Wanted) {
          continue;
        }
        ColumnEntryFill(Scratch->Fresh, ChunkX, ChunkZ, Seed);
        const ColumnEntry *Entry = ColumnCacheLookup(
            GetColumnCache(COLUMN_CACHE_MAIN_SLOT), ChunkX, ChunkZ, Seed);
        if (!CheckCube(Scratch, ChunkX, ChunkY, ChunkZ, Entry)) {
          return false;
        }
        if (Scratch->FloodedWater > Before) {
          LogInfo("WORLDGEN_TEST: ok flooded cube (%d, %d, %d) matches its "
                  "probe and has no water beside dry air",
                  ChunkX, ChunkY, ChunkZ);
          return true;
        }
        if (++Candidates >= MAX_CANDIDATES) {
          LogError("WORLDGEN_TEST: FAIL no flooded cave water in %d candidate "
                   "cubes, so the flood checks went unchecked",
                   MAX_CANDIDATES);
          return false;
        }
      }
    }
  }
  LogError("WORLDGEN_TEST: FAIL no flood candidate cube below the origin");
  return false;
}

// The fast SOLID fill must match the per-block filler where a pocket reaches
// the cube, and a cube crossing the stone-deepslate band must hold both. The
// fixed test cubes need not hit either case, so both are searched for.
static bool CheckFillerRegion(SelfTestScratch *Scratch, uint64_t Seed) {
  enum { SEARCH_RADIUS = 4, SHALLOWEST = -6, DEEPEST = -30, BAND_CHUNK_Y = -5 };
  bool FoundPocketSolid = false;
  for (int ChunkY = SHALLOWEST; ChunkY >= DEEPEST && !FoundPocketSolid; ChunkY--) {
    for (int ChunkX = -SEARCH_RADIUS; ChunkX <= SEARCH_RADIUS && !FoundPocketSolid;
         ChunkX++) {
      for (int ChunkZ = -SEARCH_RADIUS;
           ChunkZ <= SEARCH_RADIUS && !FoundPocketSolid; ChunkZ++) {
        FillerCube Filler;
        FillerCubeInit(&Filler, ChunkX, ChunkY, ChunkZ, Seed);
        if (Filler.PocketCount == 0) {
          continue;
        }
        const ColumnEntry *Entry = ColumnCacheLookup(
            GetColumnCache(COLUMN_CACHE_MAIN_SLOT), ChunkX, ChunkZ, Seed);
        static ColumnEntry SEntry;
        SEntry = *Entry;
        if (GenerateCube(Scratch->Reference, ChunkX, ChunkY, ChunkZ, &SEntry,
                         true, NULL) != CUBE_CLASS_SOLID) {
          continue;
        }
        ColumnEntryFill(Scratch->Fresh, ChunkX, ChunkZ, Seed);
        if (!CheckCube(Scratch, ChunkX, ChunkY, ChunkZ, &SEntry)) {
          return false;
        }
        LogInfo("WORLDGEN_TEST: ok SOLID cube (%d, %d, %d) with %d pocket(s) "
                "matches the per-block filler",
                ChunkX, ChunkY, ChunkZ, Filler.PocketCount);
        FoundPocketSolid = true;
      }
    }
  }
  if (!FoundPocketSolid) {
    LogError("WORLDGEN_TEST: FAIL no SOLID cube reached by a pocket, so the "
             "fast filler fill went unchecked");
    return false;
  }

  BlockID Stone = (BlockID)GetBlockIdByName("Stone");
  BlockID Deep = (BlockID)GetBlockIdByName("deepslate");
  for (int ChunkX = -SEARCH_RADIUS; ChunkX <= SEARCH_RADIUS; ChunkX++) {
    const ColumnEntry *Entry = ColumnCacheLookup(
        GetColumnCache(COLUMN_CACHE_MAIN_SLOT), ChunkX, 0, Seed);
    static ColumnEntry SEntry;
    SEntry = *Entry;
    (void)GenerateCube(Scratch->Reference, ChunkX, BAND_CHUNK_Y, 0, &SEntry,
                       true, NULL);
    int StoneCount = 0;
    int DeepCount = 0;
    for (size_t Idx = 0; Idx < CHUNK_VOLUME; Idx++) {
      BlockID Block = (&Scratch->Reference->Data[0][0][0])[Idx];
      StoneCount += (Block == Stone) ? 1 : 0;
      DeepCount += (Block == Deep) ? 1 : 0;
    }
    if (StoneCount > 0 && DeepCount > 0) {
      LogInfo("WORLDGEN_TEST: ok band cube (%d, %d, 0) holds %d stone and %d "
              "deepslate blocks",
              ChunkX, BAND_CHUNK_Y, StoneCount, DeepCount);
      return true;
    }
  }
  LogError("WORLDGEN_TEST: FAIL no cube in the stone-deepslate band holds "
           "both blocks");
  return false;
}

static bool RunChecks(SelfTestScratch *Scratch) {
  enum { PREPARED_SAMPLES = 512 };
  if (!CheckSplines()) {
    return false;
  }
  if (!NoisePreparedMatches(PREPARED_SAMPLES)) {
    LogError("WORLDGEN_TEST: FAIL a noise field prepared for the world seed "
             "samples differently from one prepared afresh");
    return false;
  }
  LogInfo("WORLDGEN_TEST: ok prepared noise fields match fresh preparation at "
          "%d samples each",
          PREPARED_SAMPLES);

  uint64_t Seed = GetWorldSeed();
  for (int Index = 0; Index < TEST_COLUMN_COUNT; Index++) {
    int ColumnX = 0;
    int ColumnZ = 0;
    GetTestColumn(Index, &ColumnX, &ColumnZ);
    if (!CheckColumn(Scratch, ColumnX, ColumnZ, Seed)) {
      return false;
    }
  }

  if (!CheckStructureRegion(Scratch, Seed) ||
      !CheckFloodRegion(Scratch, Seed) || !CheckFillerRegion(Scratch, Seed)) {
    return false;
  }

  for (int Class = 0; Class < CUBE_CLASS_COUNT; Class++) {
    if (Scratch->ClassCounts[Class] == 0) {
      LogError("WORLDGEN_TEST: FAIL no test cube was classified %s, so its "
               "short-circuit went unchecked",
               CLASS_NAMES[Class]);
      return false;
    }
  }
  LogInfo("WORLDGEN_TEST: ok every class exercised (AIR=%d SOLID=%d MIXED=%d)",
          Scratch->ClassCounts[CUBE_CLASS_AIR],
          Scratch->ClassCounts[CUBE_CLASS_SOLID],
          Scratch->ClassCounts[CUBE_CLASS_MIXED]);

  if (Scratch->ProbedBlocks == 0) {
    LogError("WORLDGEN_TEST: FAIL no block was probed, so probe equality went "
             "unchecked");
    return false;
  }
  LogInfo("WORLDGEN_TEST: ok density probe equals the generator at %d blocks",
          Scratch->ProbedBlocks);
  LogInfo("WORLDGEN_TEST: ok %d biome map cells equal the pure sampler",
          Scratch->BiomeCells);
  LogInfo("WORLDGEN_TEST: ok fluids match the probe at %d blocks; %d flooded "
          "water blocks, none beside dry air",
          Scratch->FluidProbes, Scratch->FloodedWater);
  LogInfo("WORLDGEN_TEST: ok 3D terrain noise within bound at %d corners (max "
          "|n3| = %.4f, bound %.2f)",
          Scratch->NoiseSamples, (double)Scratch->MaxNoise,
          (double)TERRAIN_NOISE_BOUND);
  return true;
}

bool RunWorldgenSelfTest(void) {
  SelfTestScratch Scratch = {
      .Reference = (Chunk *)calloc(1, sizeof(Chunk)),
      .PerBlock = (Chunk *)calloc(1, sizeof(Chunk)),
      .Uncached = (Chunk *)calloc(1, sizeof(Chunk)),
      .Fresh = (ColumnEntry *)calloc(1, sizeof(ColumnEntry)),
      .Density = (float *)calloc(CHUNK_VOLUME, sizeof(float)),
      .ClassCounts = {0},
      .ProbedBlocks = 0,
      .NoiseSamples = 0,
      .MaxNoise = 0.0F,
      .FluidProbes = 0,
      .FloodedWater = 0,
      .BiomeCells = 0};

  bool Passed = false;
  if (Scratch.Reference == NULL || Scratch.PerBlock == NULL ||
      Scratch.Uncached == NULL || Scratch.Fresh == NULL ||
      Scratch.Density == NULL) {
    LogError("WORLDGEN_TEST: FAIL out of memory allocating scratch chunks");
  } else {
    Passed = RunChecks(&Scratch);
  }

  free(Scratch.Reference);
  free(Scratch.PerBlock);
  free(Scratch.Uncached);
  free(Scratch.Fresh);
  free(Scratch.Density);
  return Passed;
}
