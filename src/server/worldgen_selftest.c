#include "server/worldgen_selftest.h"
#include "core/log.h"
#include "persistence/world_save.h"
#include "world/chunk.h"
#include "world/feature.h"
#include "world/worldgen_column_cache.h"
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
  int ClassCounts[CUBE_CLASS_COUNT];
} SelfTestScratch;

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
      bool SameTemperature = Left->Temperature[CellX][CellZ] ==
                             Right->Temperature[CellX][CellZ];
      bool SameHumidity =
          Left->Humidity[CellX][CellZ] == Right->Humidity[CellX][CellZ];
      if (!SameTemperature || !SameHumidity) {
        return false;
      }
    }
  }
  return true;
}

static bool EntriesEqual(const ColumnEntry *Left, const ColumnEntry *Right) {
  return Left->ColumnX == Right->ColumnX && Left->ColumnZ == Right->ColumnZ &&
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
static CubeClass GenerateCube(Chunk *Out, int ChunkX, int ChunkY, int ChunkZ,
                              const ColumnEntry *Column, bool Classify) {
  Out->ChunkX = ChunkX;
  Out->ChunkY = ChunkY;
  Out->ChunkZ = ChunkZ;

  const TerrainGenOptions OPTIONS = {.AllowClassification = Classify,
                                     .CountClasses = false};
  FillChunkBiomeMap(Out, Column);
  CubeClass Class = GenerateChunkTerrainEx(Out, Column, &OPTIONS);
  PlaceChunkFeatures(Out);
  PlaceChunkFlora(Out, Column);
  return Class;
}

static bool CheckCube(SelfTestScratch *Scratch, int ChunkX, int ChunkY,
                      int ChunkZ, const ColumnEntry *Cached) {
  CubeClass Class =
      GenerateCube(Scratch->Reference, ChunkX, ChunkY, ChunkZ, Cached, true);
  Scratch->ClassCounts[Class]++;

  (void)GenerateCube(Scratch->PerBlock, ChunkX, ChunkY, ChunkZ, Cached, false);
  if (!ChunksMatch(Scratch->Reference, Scratch->PerBlock)) {
    LogError("WORLDGEN_TEST: FAIL cube (%d, %d, %d) classified %s differs from "
             "the per-block path",
             ChunkX, ChunkY, ChunkZ, CLASS_NAMES[Class]);
    return false;
  }

  (void)GenerateCube(Scratch->Uncached, ChunkX, ChunkY, ChunkZ, Scratch->Fresh,
                     true);
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
  if (!EntriesEqual(Cached, Scratch->Fresh)) {
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

static bool RunChecks(SelfTestScratch *Scratch) {
  uint64_t Seed = GetWorldSeed();
  for (int Index = 0; Index < TEST_COLUMN_COUNT; Index++) {
    int ColumnX = 0;
    int ColumnZ = 0;
    GetTestColumn(Index, &ColumnX, &ColumnZ);
    if (!CheckColumn(Scratch, ColumnX, ColumnZ, Seed)) {
      return false;
    }
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
  return true;
}

bool RunWorldgenSelfTest(void) {
  SelfTestScratch Scratch = {
      .Reference = (Chunk *)calloc(1, sizeof(Chunk)),
      .PerBlock = (Chunk *)calloc(1, sizeof(Chunk)),
      .Uncached = (Chunk *)calloc(1, sizeof(Chunk)),
      .Fresh = (ColumnEntry *)calloc(1, sizeof(ColumnEntry)),
      .ClassCounts = {0}};

  bool Passed = false;
  if (Scratch.Reference == NULL || Scratch.PerBlock == NULL ||
      Scratch.Uncached == NULL || Scratch.Fresh == NULL) {
    LogError("WORLDGEN_TEST: FAIL out of memory allocating scratch chunks");
  } else {
    Passed = RunChecks(&Scratch);
  }

  free(Scratch.Reference);
  free(Scratch.PerBlock);
  free(Scratch.Uncached);
  free(Scratch.Fresh);
  return Passed;
}
