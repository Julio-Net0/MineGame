#include "server/lod_selftest.h"

#include "core/log.h"
#include "persistence/world_save.h"
#include "world/block_system.h"
#include "world/chunk.h"
#include "world/chunk_snapshot.h"
#include "world/feature.h"
#include "world/lod/lod_downsample.h"
#include "world/lod/lod_section.h"
#include "world/lod/lod_store.h"
#include "world/lod/lod_voxel.h"
#include "world/surface.h"
#include "world/worldgen_column_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TEST_CHUNKS = 8,
  TEST_PERMUTATIONS = 6,
  ORDER_HASH_LEVELS = 3,
  // The 2x2x2 group the rule cases build: input base (4, 4, 4), output (2, 2, 2).
  GROUP_BASE = 4,
  GROUP_OUT = 2,
  GROUP_MEMBERS = 8,
  TOP_GROUP_BASE = 14,
  TOP_GROUP_OUT = 7,
  // Synthetic terrain: heights GROUND_BASE..GROUND_BASE+GROUND_RANGE-1 over a
  // 32-block tall pair of chunk layers, with water up to SYN_SEA.
  GROUND_BASE = 8,
  GROUND_RANGE = 14,
  SYN_SEA = 12,
  SOIL_DEPTH = 3,
  CANOPY_LOW = 2,
  CANOPY_HIGH = 4,
  CANOPY_SPACING = 3,
  CANOPY_ODDS = 5,
  FLOWER_ODDS = 4,
  BIOME_PATCH_SHIFT = 3,
  BIOME_VARIANTS = 4,
  // Clipmap check.
  SMALL_RADIUS = 2,
  FAR_CHUNK = 100000,
  OUT_OF_RANGE_CHUNK = 1000,
  EVICT_SPAN = 1,
  CHECK_TEXT_CHARS = 96,
  BYTE_BITS = 8
};

// Combined hash of levels 1-3 of the synthetic 2x2x2 chunk block. Changes only
// when the downsampling rule, the storage hash or the block ids change; update
// it deliberately in the same change.
#define LOD_EXPECTED_SYNTHETIC_HASH 0xC03B63FDD1E8CC1BULL

#define LCG_MULTIPLIER 6364136223846793005ULL
#define LCG_INCREMENT 1442695040888963407ULL
#define COLUMN_MIX_X 0x9E3779B1U
#define COLUMN_MIX_Z 0x85EBCA77U
#define COLUMN_MIX_SHIFT 15U
#define LCG_OUTPUT_SHIFT 33U
#define COMBINE_MULTIPLIER 0x100000001B3ULL

typedef struct {
  BlockID Stone;
  BlockID Dirt;
  BlockID Grass;
  BlockID Sand;
  BlockID Water;
  BlockID Leaves;
  BlockID Glass;
  BlockID Flower;
  BlockID TallGrass;
} TestBlocks;

typedef struct {
  TestBlocks Blocks;
  LodVoxel *Grid;
  LodPatch8 *Synthetic;
  LodPatch8 *Real;
  Chunk *Generated;
  ChunkSnapshot *Snapshot;
  int RealBase[3];
} LodTestScratch;

static bool Check(bool Condition, const char *What) {
  if (!Condition) {
    LogError("LOD_TEST: FAIL %s", What);
  }
  return Condition;
}

static void Ok(const char *What) { LogInfo("LOD_TEST: ok %s", What); }

static bool ResolveOne(const char *Name, BlockID *Out) {
  int Id = GetBlockIdByName(Name);
  if (Id <= 0) {
    LogError("LOD_TEST: FAIL block \"%s\" is not registered", Name);
    return false;
  }
  *Out = (BlockID)Id;
  return true;
}

static bool ResolveBlocks(TestBlocks *Out) {
  return ResolveOne("Stone", &Out->Stone) && ResolveOne("Dirt", &Out->Dirt) &&
         ResolveOne("Grass", &Out->Grass) && ResolveOne("Sand", &Out->Sand) &&
         ResolveOne("Water", &Out->Water) &&
         ResolveOne("Leaves", &Out->Leaves) &&
         ResolveOne("Glass", &Out->Glass) &&
         ResolveOne("allium", &Out->Flower) &&
         ResolveOne("TallGrass", &Out->TallGrass);
}

static LodVoxel Solid(BlockID Id) { return LodCanonicalVoxel(Id, 0); }

static void FillGrid(LodVoxel *Grid, LodVoxel Value) {
  for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
    Grid[Idx] = Value;
  }
}

static void SetMember(LodVoxel *Grid, int Base, int Member, LodVoxel Value) {
  Grid[LodVoxelIndex(Base + (Member & 1), Base + ((Member >> 1) & 1),
                     Base + ((Member >> 2) & 1))] = Value;
}

static LodVoxel ReduceAt(const LodVoxel *Grid, int OutX, int OutY, int OutZ) {
  LodPatch8 Patch;
  LodDownsampleVoxels(Grid, &Patch);
  return Patch.Voxels[LodPatchIndex(OutX, OutY, OutZ)];
}

// --- voxel format and classes -------------------------------------------

static bool TestVoxelFormat(void) {
  enum { SAMPLE_ID = 0xABCD, SAMPLE_BIOME = 0x5A, SAMPLE_SKY = 7, SAMPLE_BL = 3 };
  LodVoxel Packed = LodVoxelPack(SAMPLE_ID, SAMPLE_BIOME, SAMPLE_SKY, SAMPLE_BL);
  bool Passed =
      Check(LodVoxelBlock(Packed) == SAMPLE_ID &&
                LodVoxelBiome(Packed) == SAMPLE_BIOME &&
                LodVoxelSkyLight(Packed) == SAMPLE_SKY &&
                LodVoxelBlockLight(Packed) == SAMPLE_BL,
            "voxel pack/unpack round trip") &&
      Check(LodVoxelSkyLight(LodVoxelMake(1, 0)) == LOD_VOXEL_DEFAULT_SKY_LIGHT &&
                LodVoxelBlockLight(LodVoxelMake(1, 0)) ==
                    LOD_VOXEL_DEFAULT_BLOCK_LIGHT,
            "voxel default light") &&
      Check(LodVoxelBlock(LOD_VOXEL_AIR) == BLOCK_ID_AIR &&
                LodVoxelBiome(LOD_VOXEL_AIR) == 0,
            "canonical air voxel");
  if (Passed) {
    Ok("voxel format (16b id, 8b biome, 4b sky, 4b block light)");
  }
  return Passed;
}

static bool TestClasses(const TestBlocks *B) {
  bool Passed =
      Check(LodClassifyBlock(BLOCK_ID_AIR) == LOD_CLASS_AIR, "air class") &&
      Check(LodClassifyBlock(B->Stone) == LOD_CLASS_OPAQUE &&
                LodClassifyBlock(B->Dirt) == LOD_CLASS_OPAQUE &&
                LodClassifyBlock(B->Grass) == LOD_CLASS_OPAQUE,
            "opaque class") &&
      Check(LodClassifyBlock(B->Water) == LOD_CLASS_WATER, "water class") &&
      Check(LodClassifyBlock(B->Leaves) == LOD_CLASS_LEAVES &&
                LodClassifyBlock(B->Glass) == LOD_CLASS_LEAVES,
            "see-through solid class") &&
      Check(LodClassifyBlock(B->Flower) == LOD_CLASS_AIR &&
                LodClassifyBlock(B->TallGrass) == LOD_CLASS_AIR &&
                LodCanonicalVoxel(B->Flower, 3) == LOD_VOXEL_AIR,
            "cross plants become air");
  if (Passed) {
    Ok("block classes from the registry");
  }
  return Passed;
}

// --- the 2x2x2 rule --------------------------------------------------------

static bool ExpectGroup(LodVoxel *Grid, LodVoxel Expected, const char *What) {
  return Check(ReduceAt(Grid, GROUP_OUT, GROUP_OUT, GROUP_OUT) == Expected, What);
}

static bool TestClassPriority(const TestBlocks *B, LodVoxel *Grid) {
  FillGrid(Grid, LOD_VOXEL_AIR);
  for (int M = 0; M < GROUP_MEMBERS; M++) {
    SetMember(Grid, GROUP_BASE, M, Solid(B->Water));
  }
  SetMember(Grid, GROUP_BASE, 0, Solid(B->Stone));
  if (!ExpectGroup(Grid, Solid(B->Stone), "1 stone + 7 water gives stone")) {
    return false;
  }

  for (int M = 0; M < GROUP_MEMBERS; M++) {
    SetMember(Grid, GROUP_BASE, M, Solid(B->Leaves));
  }
  SetMember(Grid, GROUP_BASE, GROUP_MEMBERS - 1, Solid(B->Water));
  if (!ExpectGroup(Grid, Solid(B->Water), "1 water + 7 leaves gives water")) {
    return false;
  }

  FillGrid(Grid, LOD_VOXEL_AIR);
  SetMember(Grid, GROUP_BASE, 3, Solid(B->Leaves));
  if (!ExpectGroup(Grid, Solid(B->Leaves), "1 leaves + 7 air gives leaves")) {
    return false;
  }

  FillGrid(Grid, LOD_VOXEL_AIR);
  for (int M = 0; M < GROUP_MEMBERS; M++) {
    SetMember(Grid, GROUP_BASE, M,
              LodCanonicalVoxel((M & 1) != 0 ? B->Flower : B->TallGrass, 1));
  }
  if (!ExpectGroup(Grid, LOD_VOXEL_AIR, "a group of cross plants gives air")) {
    return false;
  }
  Ok("class priority opaque > water > leaves > air");
  return true;
}

// Columns of Below up to SurfaceY - 1, Top at SurfaceY, air above.
static void FillLayered(LodVoxel *Grid, LodVoxel Below, LodVoxel Top,
                        int SurfaceY) {
  for (int X = 0; X < LOD_SECTION_SIZE; X++) {
    for (int Y = 0; Y < LOD_SECTION_SIZE; Y++) {
      for (int Z = 0; Z < LOD_SECTION_SIZE; Z++) {
        LodVoxel Value = LOD_VOXEL_AIR;
        if (Y < SurfaceY) {
          Value = Below;
        } else if (Y == SurfaceY) {
          Value = Top;
        }
        Grid[LodVoxelIndex(X, Y, Z)] = Value;
      }
    }
  }
}

static bool TestThinSurface(const TestBlocks *B, LodVoxel *Grid) {
  FillLayered(Grid, Solid(B->Dirt), Solid(B->Grass), GROUP_BASE + 1);
  if (!ExpectGroup(Grid, Solid(B->Grass),
                   "4 dirt (y=0) + 4 grass (y=1) gives grass") ||
      !Check(ReduceAt(Grid, GROUP_OUT, 1, GROUP_OUT) == Solid(B->Dirt),
             "buried dirt group stays dirt")) {
    return false;
  }

  FillLayered(Grid, Solid(B->Stone), Solid(B->Sand), GROUP_BASE + 1);
  if (!ExpectGroup(Grid, Solid(B->Sand), "4 stone (y=0) + 4 sand (y=1) gives sand")) {
    return false;
  }

  // At the grid's top edge nothing above is known; height alone decides.
  FillLayered(Grid, Solid(B->Dirt), Solid(B->Grass), TOP_GROUP_BASE + 1);
  if (!Check(ReduceAt(Grid, GROUP_OUT, TOP_GROUP_OUT, GROUP_OUT) ==
                 Solid(B->Grass),
             "grass on the grid's top layer gives grass")) {
    return false;
  }

  // Buried and evenly split: frequency ties, so the lower block id wins.
  FillGrid(Grid, Solid(B->Stone));
  for (int M = 0; M < GROUP_MEMBERS; M++) {
    SetMember(Grid, GROUP_BASE, M, Solid((M & 1) != 0 ? B->Dirt : B->Stone));
  }
  BlockID Lower = (B->Dirt < B->Stone) ? B->Dirt : B->Stone;
  if (!ExpectGroup(Grid, Solid(Lower), "buried 4/4 tie gives the lower id")) {
    return false;
  }
  Ok("thin surface layers survive (grass over dirt, sand over stone)");
  return true;
}

// --- storage ---------------------------------------------------------------

typedef struct {
  int Distinct;
  LodStorageMode Mode;
} StorageCase;

static size_t ExpectedHeap(const StorageCase *Case) {
  switch (Case->Mode) {
  case LOD_STORAGE_PACKED4:
    return ((size_t)Case->Distinct * sizeof(LodVoxel)) + (LOD_SECTION_VOLUME / 2);
  case LOD_STORAGE_PACKED8:
    return ((size_t)Case->Distinct * sizeof(LodVoxel)) + LOD_SECTION_VOLUME;
  case LOD_STORAGE_DENSE:
    return sizeof(LodVoxel) * LOD_SECTION_VOLUME;
  case LOD_STORAGE_UNIFORM:
  default:
    return 0;
  }
}

static void FillDistinct(LodVoxel *Grid, int Distinct) {
  for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
    Grid[Idx] = LodVoxelMake((BlockID)(1 + (Idx % Distinct)),
                             (unsigned)(Idx % Distinct) & LOD_VOXEL_BIOME_MASK);
  }
}

static bool SectionMatches(const LodSection *Section, const LodVoxel *Grid) {
  for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
    int X = Idx / (LOD_SECTION_SIZE * LOD_SECTION_SIZE);
    int Y = (Idx / LOD_SECTION_SIZE) % LOD_SECTION_SIZE;
    int Z = Idx % LOD_SECTION_SIZE;
    if (LodSectionGet(Section, X, Y, Z) != Grid[Idx]) {
      return false;
    }
  }
  return true;
}

static bool TestStorage(LodVoxel *Grid) {
  static const StorageCase CASES[] = {
      {1, LOD_STORAGE_UNIFORM},  {2, LOD_STORAGE_PACKED4},
      {16, LOD_STORAGE_PACKED4}, {17, LOD_STORAGE_PACKED8},
      {256, LOD_STORAGE_PACKED8}, {257, LOD_STORAGE_DENSE},
      {LOD_SECTION_VOLUME, LOD_STORAGE_DENSE}};
  int CaseCount = (int)(sizeof(CASES) / sizeof(CASES[0]));

  LodSection Section;
  LodSectionInit(&Section);
  // Up through every form and back down again on one section, so each
  // transition between forms is exercised in both directions.
  for (int Step = 0; Step < (2 * CaseCount) - 1; Step++) {
    int CaseIdx = (Step < CaseCount) ? Step : (2 * CaseCount) - 2 - Step;
    const StorageCase *Case = &CASES[CaseIdx];
    FillDistinct(Grid, Case->Distinct);
    char What[CHECK_TEXT_CHARS];
    (void)snprintf(What, sizeof(What), "storage with %d distinct values",
                   Case->Distinct);
    if (!Check(LodSectionEncode(&Section, Grid), What) ||
        !Check(Section.Mode == Case->Mode, What) ||
        !Check(LodSectionHeapBytes(&Section) == ExpectedHeap(Case), What) ||
        !Check(SectionMatches(&Section, Grid), What)) {
      LodSectionFree(&Section);
      return false;
    }
  }
  LodSectionFree(&Section);
  Ok("storage round trip uniform <-> packed4 <-> packed8 <-> dense");
  return true;
}

// --- synthetic terrain -----------------------------------------------------

static uint32_t ColumnNoise(int X, int Z) {
  uint32_t Hash = ((uint32_t)X * COLUMN_MIX_X) ^ ((uint32_t)Z * COLUMN_MIX_Z);
  return Hash ^ (Hash >> COLUMN_MIX_SHIFT);
}

static BlockID SyntheticBlock(const TestBlocks *B, int X, int Y, int Z) {
  uint32_t Noise = ColumnNoise(X, Z);
  int Height = GROUND_BASE + (int)(Noise % (uint32_t)GROUND_RANGE);
  if (Y < Height - SOIL_DEPTH) {
    return B->Stone;
  }
  if (Y < Height) {
    return B->Dirt;
  }
  if (Y == Height) {
    return (Height <= SYN_SEA) ? B->Sand : B->Grass;
  }
  if (Y <= SYN_SEA) {
    return B->Water;
  }
  if (Y == Height + 1 && (Noise >> BYTE_BITS) % FLOWER_ODDS == 0U) {
    return B->TallGrass;
  }
  bool Canopy = ColumnNoise(X / CANOPY_SPACING, Z / CANOPY_SPACING) %
                    CANOPY_ODDS ==
                0U;
  if (Canopy && Y >= Height + CANOPY_LOW && Y <= Height + CANOPY_HIGH) {
    return B->Leaves;
  }
  return BLOCK_ID_AIR;
}

static void SyntheticChunkPatch(const TestBlocks *B, LodVoxel *Grid, int ChunkX,
                                int ChunkY, int ChunkZ, LodPatch8 *Out) {
  for (int X = 0; X < CHUNK_SIZE; X++) {
    for (int Y = 0; Y < CHUNK_SIZE; Y++) {
      for (int Z = 0; Z < CHUNK_SIZE; Z++) {
        int Wx = (ChunkX * CHUNK_SIZE) + X;
        int Wy = (ChunkY * CHUNK_SIZE) + Y;
        int Wz = (ChunkZ * CHUNK_SIZE) + Z;
        unsigned Biome = (unsigned)(((Wx >> BIOME_PATCH_SHIFT) +
                                     (Wz >> BIOME_PATCH_SHIFT)) &
                                    (BIOME_VARIANTS - 1));
        Grid[LodVoxelIndex(X, Y, Z)] =
            LodCanonicalVoxel(SyntheticBlock(B, Wx, Wy, Wz), Biome);
      }
    }
  }
  LodDownsampleVoxels(Grid, Out);
}

static void ChunkOfIndex(const int Base[3], int Idx, int Out[3]) {
  Out[0] = Base[0] + (Idx & 1);
  Out[1] = Base[1] + ((Idx >> 1) & 1);
  Out[2] = Base[2] + ((Idx >> 2) & 1);
}

static uint64_t StoreHash(const LodStore *Store, int MaxLevel) {
  uint64_t Hash = 0;
  for (int Level = 1; Level <= MaxLevel; Level++) {
    Hash = (Hash ^ LodStoreLevelHash(Store, Level)) * COMBINE_MULTIPLIER;
  }
  return Hash;
}

static void MakeOrder(int Permutation, int Order[TEST_CHUNKS]) {
  for (int Idx = 0; Idx < TEST_CHUNKS; Idx++) {
    Order[Idx] = (Permutation == 1) ? TEST_CHUNKS - 1 - Idx : Idx;
  }
  if (Permutation < 2) {
    return;
  }
  uint64_t State = (uint64_t)Permutation;
  for (int Idx = TEST_CHUNKS - 1; Idx > 0; Idx--) {
    State = (State * LCG_MULTIPLIER) + LCG_INCREMENT;
    int Pick = (int)((State >> LCG_OUTPUT_SHIFT) % (uint64_t)(Idx + 1));
    int Swap = Order[Idx];
    Order[Idx] = Order[Pick];
    Order[Pick] = Swap;
  }
}

// Fresh store, the 8 patches written in the permutation's order.
static bool IngestPermutation(const LodPatch8 *Patches, const int Base[3],
                              int Permutation, LodStore *Out) {
  LodStoreConfig Config;
  LodStoreDefaultConfig(&Config);
  if (!LodStoreInit(Out, &Config)) {
    return Check(false, "store allocation");
  }
  int Order[TEST_CHUNKS];
  MakeOrder(Permutation, Order);
  for (int Idx = 0; Idx < TEST_CHUNKS; Idx++) {
    int ChunkPos[3];
    ChunkOfIndex(Base, Order[Idx], ChunkPos);
    LodWriteResult Result = LodStoreWriteChunk(
        Out, ChunkPos[0], ChunkPos[1], ChunkPos[2], &Patches[Order[Idx]],
        LOD_ORIGIN_INGESTED);
    if (!Check(Result == LOD_WRITE_APPLIED, "ingest write applied")) {
      LodStoreFree(Out);
      return false;
    }
  }
  return true;
}

// Every permutation must give the identity order's hash at levels 1..Levels.
static bool CheckOrderIndependence(const LodPatch8 *Patches, const int Base[3],
                                   int Levels, uint64_t *OutHash) {
  uint64_t Reference = 0;
  for (int Perm = 0; Perm < TEST_PERMUTATIONS; Perm++) {
    LodStore Store;
    if (!IngestPermutation(Patches, Base, Perm, &Store)) {
      return false;
    }
    uint64_t Hash = StoreHash(&Store, Levels);
    LodStoreFree(&Store);
    if (Perm == 0) {
      Reference = Hash;
    } else if (Hash != Reference) {
      LogError("LOD_TEST: FAIL ingestion order %d gives hash %016llx, identity "
               "gives %016llx",
               Perm, (unsigned long long)Hash, (unsigned long long)Reference);
      return false;
    }
  }
  *OutHash = Reference;
  return true;
}

static bool TestSyntheticHash(LodTestScratch *S) {
  static const int BASE[3] = {0, 0, 0};
  for (int Idx = 0; Idx < TEST_CHUNKS; Idx++) {
    int ChunkPos[3];
    ChunkOfIndex(BASE, Idx, ChunkPos);
    SyntheticChunkPatch(&S->Blocks, S->Grid, ChunkPos[0], ChunkPos[1], ChunkPos[2],
                        &S->Synthetic[Idx]);
  }
  uint64_t Hash = 0;
  if (!CheckOrderIndependence(S->Synthetic, BASE, ORDER_HASH_LEVELS, &Hash)) {
    return false;
  }
  if (Hash != LOD_EXPECTED_SYNTHETIC_HASH) {
    LogError("LOD_TEST: FAIL synthetic hash %016llx, expected %016llx",
             (unsigned long long)Hash,
             (unsigned long long)LOD_EXPECTED_SYNTHETIC_HASH);
    return false;
  }
  LogInfo("LOD_TEST: ok fixed synthetic hash %016llx over levels 1-3, same for "
          "%d ingestion orders",
          (unsigned long long)Hash, TEST_PERMUTATIONS);
  return true;
}

// --- real terrain ------------------------------------------------------------

static bool GenerateRealChunk(Chunk *Out, int ChunkX, int ChunkY, int ChunkZ,
                              uint64_t Seed) {
  const ColumnEntry *Column = ColumnCacheLookup(
      GetColumnCache(COLUMN_CACHE_MAIN_SLOT), ChunkX, ChunkZ, Seed);
  if (Column == NULL) {
    return false;
  }
  // A private copy: the feature pass looks up neighbouring columns in the
  // main-thread cache, which may reuse the slot Column points into.
  static ColumnEntry SLocal;
  SLocal = *Column;
  Out->ChunkX = ChunkX;
  Out->ChunkY = ChunkY;
  Out->ChunkZ = ChunkZ;
  const TerrainGenOptions OPTIONS = {.AllowClassification = true,
                                     .CountClasses = false,
                                     .DebugDensity = (float *)0,
                                     .CornerRows = (struct CornerRowRing *)0};
  FillChunkBiomeMap(Out, &SLocal);
  (void)GenerateChunkTerrainEx(Out, &SLocal, &OPTIONS);
  PlaceChunkFeatures(Out, GetColumnCache(COLUMN_CACHE_MAIN_SLOT), true);
  PlaceChunkFlora(Out, &SLocal);
  return true;
}

// The snapshot the mesher would capture, interior only (margins stay air).
static void SnapshotFromChunk(const Chunk *Source, ChunkSnapshot *Out) {
  memset(Out, 0, sizeof(*Out));
  Out->ChunkX = Source->ChunkX;
  Out->ChunkY = Source->ChunkY;
  Out->ChunkZ = Source->ChunkZ;
  for (int X = 0; X < CHUNK_SIZE; X++) {
    for (int Y = 0; Y < CHUNK_SIZE; Y++) {
      for (int Z = 0; Z < CHUNK_SIZE; Z++) {
        Out->Blocks[X + 1][Y + 1][Z + 1] = ChunkGetLocal(Source, X, Y, Z);
      }
    }
  }
  for (int Cx = 0; Cx < BIOME_CELLS_PER_CHUNK; Cx++) {
    for (int Cy = 0; Cy < BIOME_CELLS_PER_CHUNK; Cy++) {
      for (int Cz = 0; Cz < BIOME_CELLS_PER_CHUNK; Cz++) {
        Out->BiomeCells[Cx + 1][Cy][Cz + 1] = Source->BiomeMap[Cx][Cy][Cz];
        Out->BiomeCellPresent[Cx + 1][Cy][Cz + 1] = 1;
      }
    }
  }
}

static void PatchFromChunk(const Chunk *Source, LodVoxel *Grid, LodPatch8 *Out) {
  for (int X = 0; X < CHUNK_SIZE; X++) {
    for (int Y = 0; Y < CHUNK_SIZE; Y++) {
      for (int Z = 0; Z < CHUNK_SIZE; Z++) {
        Grid[LodVoxelIndex(X, Y, Z)] = LodCanonicalVoxel(
            ChunkGetLocal(Source, X, Y, Z),
            GetChunkBiomeAtLocal(Source, X, Y, Z));
      }
    }
  }
  LodDownsampleVoxels(Grid, Out);
}

// An odd base puts the 2x2x2 block across level-1 section borders, so the
// order test covers writes into several sections and their parents.
static int OddBaseCovering(int Coord) { return (LodFloorShift(Coord - 1, 1) * 2) + 1; }

static bool BuildRealPatches(LodTestScratch *S) {
  uint64_t Seed = GetWorldSeed();
  SpawnPoint Spawn = FindSpawnColumn(Seed);
  S->RealBase[0] = OddBaseCovering(LodFloorShift(Spawn.X, 4));
  S->RealBase[1] = OddBaseCovering(LodFloorShift(Spawn.Surface, 4));
  S->RealBase[2] = OddBaseCovering(LodFloorShift(Spawn.Z, 4));

  int SolidBlocks = 0;
  for (int Idx = 0; Idx < TEST_CHUNKS; Idx++) {
    int ChunkPos[3];
    ChunkOfIndex(S->RealBase, Idx, ChunkPos);
    if (!Check(GenerateRealChunk(S->Generated, ChunkPos[0], ChunkPos[1], ChunkPos[2], Seed),
               "real chunk generation")) {
      return false;
    }
    SolidBlocks += S->Generated->SolidBlockCount;
    PatchFromChunk(S->Generated, S->Grid, &S->Real[Idx]);

    LodPatch8 ViaSnapshot;
    SnapshotFromChunk(S->Generated, S->Snapshot);
    LodDownsampleSnapshot(S->Snapshot, &ViaSnapshot);
    if (!Check(memcmp(&ViaSnapshot, &S->Real[Idx], sizeof(LodPatch8)) == 0,
               "snapshot path equals the chunk path")) {
      return false;
    }
  }
  LogInfo("LOD_TEST: ok 8 real chunks at (%d, %d, %d) near spawn, %d solid "
          "blocks, snapshot path equals chunk path",
          S->RealBase[0], S->RealBase[1], S->RealBase[2], SolidBlocks);
  return true;
}

static void LogStoreStats(const LodStore *Store, const char *Label) {
  LodStoreStats Stats;
  LodStoreGetStats(Store, &Stats);
  char Levels[CHECK_TEXT_CHARS] = {0};
  size_t Used = 0;
  for (int Level = 1; Level <= LOD_MAX_LEVEL && Used < sizeof(Levels); Level++) {
    int Wrote = snprintf(Levels + Used, sizeof(Levels) - Used, "%sL%d %d",
                         (Level > 1) ? ", " : "", Level,
                         Stats.SectionsPerLevel[Level]);
    if (Wrote < 0) {
      break;
    }
    Used += (size_t)Wrote;
  }
  LogInfo("LOD_TEST: %s: %d sections (%s); uniform %d, packed4 %d, packed8 %d, "
          "dense %d; %zu bytes (%zu sections, %zu table)",
          Label, Stats.Sections, Levels,
          Stats.SectionsPerMode[LOD_STORAGE_UNIFORM],
          Stats.SectionsPerMode[LOD_STORAGE_PACKED4],
          Stats.SectionsPerMode[LOD_STORAGE_PACKED8],
          Stats.SectionsPerMode[LOD_STORAGE_DENSE], Stats.TotalBytes,
          Stats.SectionBytes, Stats.TableBytes);
}

static bool TestRealOrder(LodTestScratch *S) {
  uint64_t Hash = 0;
  if (!CheckOrderIndependence(S->Real, S->RealBase, LOD_MAX_LEVEL, &Hash)) {
    return false;
  }
  LodStore Store;
  if (!IngestPermutation(S->Real, S->RealBase, 0, &Store)) {
    return false;
  }
  LogStoreStats(&Store, "real 2x2x2 chunks");
  LodStoreFree(&Store);
  LogInfo("LOD_TEST: ok real terrain hash %016llx over levels 1-%d, same for %d "
          "ingestion orders",
          (unsigned long long)Hash, LOD_MAX_LEVEL, TEST_PERMUTATIONS);
  return true;
}

// --- edits -------------------------------------------------------------------

static void FillPatch(LodPatch8 *Patch, LodVoxel Value) {
  for (int Idx = 0; Idx < LOD_PATCH_VOLUME; Idx++) {
    Patch->Voxels[Idx] = Value;
  }
}

// Voxel of the level-Level section that holds the chunk's lowest corner block.
static const LodSection *SectionOfChunk(const LodStore *Store, int Level,
                                        const int ChunkPos[3], int Voxel[3]) {
  int Section[3];
  for (int Axis = 0; Axis < 3; Axis++) {
    Section[Axis] = LodFloorShift(ChunkPos[Axis], Level);
    int Block = ChunkPos[Axis] * CHUNK_SIZE;
    Voxel[Axis] = LodFloorShift(Block, Level) - (Section[Axis] * LOD_SECTION_SIZE);
  }
  return LodStoreFind(Store, Level, Section[0], Section[1], Section[2]);
}

static bool AllUniformAir(const LodStore *Store) {
  for (int Idx = 0; Idx < Store->Capacity; Idx++) {
    const LodStoreSlot *Slot = &Store->Slots[Idx];
    if (Slot->Level > 0 && (Slot->Section->Mode != LOD_STORAGE_UNIFORM ||
                            Slot->Section->Uniform != LOD_VOXEL_AIR)) {
      return false;
    }
  }
  return true;
}

static bool TestEditPropagation(const TestBlocks *B, const int Base[3]) {
  LodPatch8 *Air = (LodPatch8 *)malloc(sizeof(LodPatch8) * TEST_CHUNKS);
  if (!Check(Air != NULL, "edit scratch allocation")) {
    return false;
  }
  for (int Idx = 0; Idx < TEST_CHUNKS; Idx++) {
    FillPatch(&Air[Idx], LOD_VOXEL_AIR);
  }
  LodStore Store;
  bool Passed = IngestPermutation(Air, Base, 0, &Store);
  if (!Passed) {
    free(Air);
    return false;
  }

  int Edited[3];
  ChunkOfIndex(Base, 0, Edited);
  uint32_t Revisions[LOD_MAX_LEVEL + 1] = {0};
  for (int Level = 1; Level <= LOD_MAX_LEVEL && Passed; Level++) {
    int Voxel[3];
    const LodSection *Section = SectionOfChunk(&Store, Level, Edited, Voxel);
    Passed = Check(Section != NULL, "every level holds the edited chunk");
    Revisions[Level] = (Section != NULL) ? Section->Revision : 0;
  }

  LodPatch8 Stone;
  FillPatch(&Stone, Solid(B->Stone));
  Passed = Passed && Check(LodStoreWriteChunk(&Store, Edited[0], Edited[1],
                                              Edited[2], &Stone,
                                              LOD_ORIGIN_INGESTED) ==
                               LOD_WRITE_APPLIED,
                           "stone edit applied");
  for (int Level = 1; Level <= LOD_MAX_LEVEL && Passed; Level++) {
    int Voxel[3];
    const LodSection *Section = SectionOfChunk(&Store, Level, Edited, Voxel);
    Passed = Check(Section->Revision == Revisions[Level] + 1U &&
                       Section->Dirty &&
                       LodSectionGet(Section, Voxel[0], Voxel[1], Voxel[2]) ==
                           Solid(B->Stone),
                   "stone edit reaches every level up to the top");
    Revisions[Level] = Section->Revision;
  }

  Passed = Passed && Check(LodStoreWriteChunk(&Store, Edited[0], Edited[1],
                                              Edited[2], &Air[0],
                                              LOD_ORIGIN_INGESTED) ==
                               LOD_WRITE_APPLIED,
                           "air edit applied");
  for (int Level = 1; Level <= LOD_MAX_LEVEL && Passed; Level++) {
    int Voxel[3];
    const LodSection *Section = SectionOfChunk(&Store, Level, Edited, Voxel);
    Passed = Check(Section->Revision == Revisions[Level] + 1U,
                   "air edit reaches every level up to the top");
  }
  Passed = Passed && Check(AllUniformAir(&Store),
                           "every section is uniform air again");
  LodStoreFree(&Store);
  free(Air);
  if (Passed) {
    Ok("edit propagates through levels 1-7 and clears back to uniform air");
  }
  return Passed;
}

// Editing one chunk of an ingested block must leave the store exactly as
// ingesting the edited data from scratch does.
static bool TestIncrementalEqualsRebuild(LodTestScratch *S) {
  LodPatch8 *Edited = (LodPatch8 *)malloc(sizeof(LodPatch8) * TEST_CHUNKS);
  if (!Check(Edited != NULL, "rebuild scratch allocation")) {
    return false;
  }
  memcpy(Edited, S->Real, sizeof(LodPatch8) * TEST_CHUNKS);
  enum { EDITED_CHUNK = 5 };
  for (int X = 0; X < LOD_PATCH_SIZE; X++) {
    for (int Y = LOD_PATCH_SIZE / 2; Y < LOD_PATCH_SIZE; Y++) {
      for (int Z = 0; Z < LOD_PATCH_SIZE; Z++) {
        Edited[EDITED_CHUNK].Voxels[LodPatchIndex(X, Y, Z)] =
            Solid(S->Blocks.Glass);
      }
    }
  }

  LodStore Incremental;
  LodStore Rebuilt;
  bool Passed = IngestPermutation(S->Real, S->RealBase, 0, &Incremental);
  if (Passed) {
    int ChunkPos[3];
    ChunkOfIndex(S->RealBase, EDITED_CHUNK, ChunkPos);
    Passed = Check(LodStoreWriteChunk(&Incremental, ChunkPos[0], ChunkPos[1], ChunkPos[2],
                                      &Edited[EDITED_CHUNK],
                                      LOD_ORIGIN_INGESTED) == LOD_WRITE_APPLIED,
                   "incremental edit applied");
    if (IngestPermutation(Edited, S->RealBase, 3, &Rebuilt)) {
      Passed = Passed && Check(StoreHash(&Incremental, LOD_MAX_LEVEL) ==
                                   StoreHash(&Rebuilt, LOD_MAX_LEVEL),
                               "incremental edit equals a rebuild");
      LodStoreFree(&Rebuilt);
    } else {
      Passed = false;
    }
    LodStoreFree(&Incremental);
  }
  free(Edited);
  if (Passed) {
    Ok("incremental edit of real terrain equals a rebuild at every level");
  }
  return Passed;
}

// --- precedence ------------------------------------------------------------

static bool TestPrecedence(LodTestScratch *S) {
  LodStoreConfig Config;
  LodStoreDefaultConfig(&Config);
  LodStore Store;
  if (!Check(LodStoreInit(&Store, &Config), "store allocation")) {
    return false;
  }
  LodPatch8 Stone;
  LodPatch8 Air;
  FillPatch(&Stone, Solid(S->Blocks.Stone));
  FillPatch(&Air, LOD_VOXEL_AIR);
  const LodPatch8 *Real = &S->Real[0];

  bool Passed =
      Check(LodStoreWriteChunk(&Store, 0, 0, 0, Real, LOD_ORIGIN_INGESTED) ==
                LOD_WRITE_APPLIED,
            "real write applied");
  uint64_t Before = StoreHash(&Store, LOD_MAX_LEVEL);
  Passed = Passed &&
           Check(LodStoreWriteChunk(&Store, 0, 0, 0, &Stone,
                                    LOD_ORIGIN_SYNTHETIC) ==
                     LOD_WRITE_SKIPPED_PRECEDENCE,
                 "synthetic write over real data is skipped") &&
           Check(StoreHash(&Store, LOD_MAX_LEVEL) == Before,
                 "skipped write changes nothing");

  const LodSection *Level1 = LodStoreFind(&Store, 1, 0, 0, 0);
  Passed = Passed &&
           Check(LodStoreWriteChunk(&Store, 1, 0, 0, &Stone,
                                    LOD_ORIGIN_SYNTHETIC) == LOD_WRITE_APPLIED,
                 "synthetic write into an empty octant applies") &&
           Check(Level1 != NULL && LodSectionGet(Level1, LOD_PATCH_SIZE, 0, 0) ==
                                       Solid(S->Blocks.Stone),
                 "synthetic data landed") &&
           Check(LodStoreWriteChunk(&Store, 1, 0, 0, &Air,
                                    LOD_ORIGIN_INGESTED) == LOD_WRITE_APPLIED,
                 "real write replaces synthetic data") &&
           Check(LodSectionGet(Level1, LOD_PATCH_SIZE, 0, 0) == LOD_VOXEL_AIR,
                 "real data landed") &&
           Check(LodStoreWriteChunk(&Store, 1, 0, 0, &Stone,
                                    LOD_ORIGIN_SYNTHETIC) ==
                     LOD_WRITE_SKIPPED_PRECEDENCE,
                 "synthetic write over replaced data is skipped");

  // A level-2 synthetic patch over the octant whose first two cells came from
  // the real chunks: only the other six cells take it.
  const LodSection *Level2 = LodStoreFind(&Store, 2, 0, 0, 0);
  LodVoxel RealCorner =
      (Level2 != NULL) ? LodSectionGet(Level2, 0, 0, 0) : LOD_VOXEL_AIR;
  Passed = Passed &&
           Check(LodStoreWritePatch(&Store, 2, 0, 0, 0, &Stone,
                                    LOD_ORIGIN_SYNTHETIC) == LOD_WRITE_APPLIED,
                 "partial synthetic level-2 write applies") &&
           Check(LodSectionGet(Level2, 0, 0, 0) == RealCorner,
                 "level-2 cell from real data kept") &&
           Check(LodSectionGet(Level2, 0, LOD_CELL_SIZE, 0) ==
                     Solid(S->Blocks.Stone),
                 "level-2 cell without real data took the synthetic patch");
  LodStoreFree(&Store);
  if (Passed) {
    Ok("real data takes precedence over synthetic data per 4^3 cell");
  }
  return Passed;
}

// --- clipmap ----------------------------------------------------------------

static bool TestClipmap(const TestBlocks *B) {
  LodStoreConfig Config;
  for (int Level = 0; Level <= LOD_MAX_LEVEL; Level++) {
    Config.Radius[Level] = SMALL_RADIUS;
    Config.VerticalRadius[Level] = SMALL_RADIUS;
  }
  LodStore Store;
  if (!Check(LodStoreInit(&Store, &Config), "store allocation")) {
    return false;
  }
  LodStoreSetAnchor(&Store, 0, 0, 0);
  LodPatch8 Stone;
  FillPatch(&Stone, Solid(B->Stone));

  bool Passed = Check(LodStoreWriteChunk(&Store, OUT_OF_RANGE_CHUNK, 0, 0, &Stone,
                                         LOD_ORIGIN_INGESTED) ==
                          LOD_WRITE_OUT_OF_RANGE,
                      "write outside the clipmap is refused");
  for (int X = -EVICT_SPAN; X <= EVICT_SPAN && Passed; X++) {
    for (int Y = -1; Y <= 1 && Passed; Y++) {
      for (int Z = -EVICT_SPAN; Z <= EVICT_SPAN && Passed; Z++) {
        Passed = Check(LodStoreWriteChunk(&Store, X, Y, Z, &Stone,
                                          LOD_ORIGIN_INGESTED) ==
                           LOD_WRITE_APPLIED,
                       "write inside the clipmap applies");
      }
    }
  }
  LodStoreStats Before;
  LodStoreGetStats(&Store, &Before);
  LogStoreStats(&Store, "clipmap before eviction");

  LodStoreSetAnchor(&Store, FAR_CHUNK, 0, FAR_CHUNK);
  LodStoreStats After;
  LodStoreGetStats(&Store, &After);
  Passed = Passed && Check(Before.Sections > 0, "clipmap filled") &&
           Check(After.Sections == 0 && After.SectionBytes == 0,
                 "moving the anchor away evicts every section") &&
           Check(After.Evicted == (uint64_t)Before.Sections,
                 "eviction count matches");
  LogStoreStats(&Store, "clipmap after eviction");
  LodStoreFree(&Store);
  Passed = Passed && Check(Store.Slots == NULL && Store.Count == 0,
                           "store released");
  if (Passed) {
    LogInfo("LOD_TEST: ok clipmap refuses out-of-range writes and evicts %d "
            "sections (%zu bytes) to zero",
            Before.Sections, Before.SectionBytes);
  }
  return Passed;
}

// ---------------------------------------------------------------------------

static void FreeScratch(LodTestScratch *S) {
  free(S->Grid);
  free(S->Synthetic);
  free(S->Real);
  free(S->Snapshot);
  if (S->Generated != NULL) {
    ChunkFreeStandalone(S->Generated);
  }
}

bool RunLodSelfTest(void) {
  LodDownsampleInit();
  LodTestScratch S = {
      .Grid = (LodVoxel *)malloc(sizeof(LodVoxel) * LOD_SECTION_VOLUME),
      .Synthetic = (LodPatch8 *)malloc(sizeof(LodPatch8) * TEST_CHUNKS),
      .Real = (LodPatch8 *)malloc(sizeof(LodPatch8) * TEST_CHUNKS),
      .Generated = ChunkCreateStandalone(),
      .Snapshot = (ChunkSnapshot *)malloc(sizeof(ChunkSnapshot))};
  bool Passed = Check(S.Grid != NULL && S.Synthetic != NULL && S.Real != NULL &&
                          S.Generated != NULL && S.Snapshot != NULL,
                      "scratch allocation") &&
                ResolveBlocks(&S.Blocks);

  Passed = Passed && TestVoxelFormat() && TestClasses(&S.Blocks) &&
           TestClassPriority(&S.Blocks, S.Grid) &&
           TestThinSurface(&S.Blocks, S.Grid) && TestStorage(S.Grid) &&
           TestSyntheticHash(&S) && BuildRealPatches(&S) && TestRealOrder(&S) &&
           TestEditPropagation(&S.Blocks, S.RealBase) &&
           TestIncrementalEqualsRebuild(&S) && TestPrecedence(&S) &&
           TestClipmap(&S.Blocks);
  FreeScratch(&S);
  return Passed;
}
