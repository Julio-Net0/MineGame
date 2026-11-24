#include "server/lod_gen_selftest.h"

#include "core/log.h"
#include "persistence/world_save.h"
#include "platform/os_time.h"
#include "world/chunk.h"
#include "world/feature.h"
#include "world/lod/lod_downsample.h"
#include "world/lod/lod_gen.h"
#include "world/lod/lod_store.h"
#include "world/surface.h"
#include "world/terrain_density.h"
#include "world/worldgen_column_cache.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Lattice probe check: LATTICE_SIDE^2 columns, LATTICE_SPACING blocks apart.
  LATTICE_SIDE = 64,
  LATTICE_SPACING = 116,
  // Columns per level of the determinism and timing pass.
  COLUMNS_PER_LEVEL = 4,
  // Agreement regions: one level-3 column (8 x 8 chunks) each.
  REGION_CHUNKS = 8,
  REGION_COUNT = 3,
  REGION_OFFSET = 5120,
  // Chunk rows kept above and below the region's terrain, so trees and the
  // floor of the lowest valley are all ingested.
  REGION_MARGIN_BLOCKS = 40,
  AGREEMENT_LEVELS = 3,
  // Voxels compared around each column's surface.
  BAND_VOXELS = 2,
  // Chunks per region axis as a shift: one level-3 section.
  REGION_SHIFT = 7,
  // Level-1 section of the synthetic-only precedence case, and its first chunk.
  SYNTH_SECTION = 5,
  SYNTH_CHUNK = SYNTH_SECTION * 2,
  OCTANTS = 8,
  PERCENT = 100,
  CHECK_TEXT_CHARS = 160
};

#define FNV_OFFSET 0xCBF29CE484222325ULL
#define FNV_PRIME 0x100000001B3ULL
#define SEED_SALT 0x5DEECE66DULL
#define MICROS_PER_SECOND 1000000.0

// Minimum agreement with the downsampled real terrain (no trees), per level:
// class of every voxel in the band around the surface, exact and +-1 voxel
// column height, and the top voxel's block where the heights agree.
static const double MIN_OCCUPANCY[AGREEMENT_LEVELS + 1] = {0.0, 95.0, 95.0, 95.0};
static const double MIN_TOP_EXACT[AGREEMENT_LEVELS + 1] = {0.0, 95.0, 95.0, 90.0};
static const double MIN_TOP_NEAR[AGREEMENT_LEVELS + 1] = {0.0, 98.0, 98.0, 98.0};
static const double MIN_SURFACE[AGREEMENT_LEVELS + 1] = {0.0, 88.0, 88.0, 82.0};
// Present parent octants whose child section the child level also emits.
static const double MIN_HIERARCHY = 98.0;

static bool Check(bool Condition, const char *What) {
  if (!Condition) {
    LogError("LOD_TEST: FAIL %s", What);
  }
  return Condition;
}

typedef struct {
  int Count;
  int Y[LOD_GEN_MAX_SECTIONS];
  uint64_t Present[LOD_GEN_MAX_SECTIONS];
  LodVoxel *Voxels;
  LodGenResult Result;
} Collected;

static void CollectSection(void *User, int SectionY,
                           const LodVoxel Voxels[LOD_SECTION_VOLUME],
                           uint64_t PresentCells) {
  Collected *Out = (Collected *)User;
  if (Out->Count >= LOD_GEN_MAX_SECTIONS) {
    return;
  }
  memcpy(Out->Voxels + ((size_t)Out->Count * LOD_SECTION_VOLUME), Voxels,
         sizeof(LodVoxel) * LOD_SECTION_VOLUME);
  Out->Y[Out->Count] = SectionY;
  Out->Present[Out->Count] = PresentCells;
  Out->Count++;
}

static bool CollectedInit(Collected *Out) {
  *Out = (Collected){0};
  Out->Voxels = (LodVoxel *)malloc(sizeof(LodVoxel) * LOD_SECTION_VOLUME *
                                   (size_t)LOD_GEN_MAX_SECTIONS);
  return Out->Voxels != NULL;
}

static void Generate(LodGenScratch *Scratch, Collected *Out, int Level, int X,
                     int Z, uint64_t Seed) {
  Out->Count = 0;
  LodGenerateColumn(Scratch, Level, X, Z, Seed, CollectSection, Out, &Out->Result);
}

static int FindCollected(const Collected *In, int SectionY) {
  for (int Idx = 0; Idx < In->Count; Idx++) {
    if (In->Y[Idx] == SectionY) {
      return Idx;
    }
  }
  return -1;
}

// The synthetic voxel at absolute voxel height Voxel of column (I, K); false
// when the generator left that height out (below its range).
static bool CollectedVoxel(const Collected *In, int I, int Voxel, int K,
                           LodVoxel *Out) {
  int SectionY = LodFloorShift(Voxel, 4);
  int Found = FindCollected(In, SectionY);
  if (Found >= 0) {
    *Out = In->Voxels[((size_t)Found * LOD_SECTION_VOLUME) +
                      (size_t)LodVoxelIndex(I, Voxel - (SectionY * LOD_SECTION_SIZE), K)];
    return true;
  }
  *Out = LOD_VOXEL_AIR;
  return SectionY > In->Result.LastY ||
         (SectionY >= In->Result.FirstY && SectionY <= In->Result.LastY);
}

static uint64_t HashWord(uint64_t Hash, uint64_t Value) {
  return (Hash ^ Value) * FNV_PRIME;
}

static uint64_t HashCollected(const Collected *In, int Level, int X, int Z) {
  uint64_t Hash = FNV_OFFSET;
  Hash = HashWord(Hash, (uint64_t)(uint32_t)Level);
  Hash = HashWord(Hash, (uint64_t)(uint32_t)X);
  Hash = HashWord(Hash, (uint64_t)(uint32_t)Z);
  for (int Idx = 0; Idx < In->Count; Idx++) {
    Hash = HashWord(Hash, (uint64_t)(uint32_t)In->Y[Idx]);
    Hash = HashWord(Hash, In->Present[Idx]);
    const LodVoxel *Voxels = In->Voxels + ((size_t)Idx * LOD_SECTION_VOLUME);
    for (int Voxel = 0; Voxel < LOD_SECTION_VOLUME; Voxel++) {
      Hash = HashWord(Hash, Voxels[Voxel]);
    }
  }
  return Hash;
}

// --- lattice probe -----------------------------------------------------------

static bool TestLatticeTop(uint64_t Seed, int CentreX, int CentreZ) {
  int Mismatches = 0;
  double FastSeconds = 0.0;
  double SlowSeconds = 0.0;
  int BaseX = LodFloorShift(CentreX, 2) * 4;
  int BaseZ = LodFloorShift(CentreZ, 2) * 4;
  int Count = LATTICE_SIDE * LATTICE_SIDE;
  int *Fast = (int *)malloc(sizeof(int) * (size_t)Count);
  if (!Check(Fast != NULL, "lattice probe scratch allocation")) {
    return false;
  }
  // Two passes rather than interleaved calls: the column shapes are memoised
  // per thread, so the second probe of a column would get them for free.
  for (int Pass = 0; Pass < 2; Pass++) {
    double Start = OsGetTimeSeconds();
    for (int Idx = 0; Idx < Count; Idx++) {
      int X = BaseX + (((Idx / LATTICE_SIDE) - (LATTICE_SIDE / 2)) * LATTICE_SPACING);
      int Z = BaseZ + (((Idx % LATTICE_SIDE) - (LATTICE_SIDE / 2)) * LATTICE_SPACING);
      if (Pass == 0) {
        Fast[Idx] = GetTerrainTopOnLattice(X, Z, Seed);
      } else if (GetTerrainTop(X, Z, Seed) != Fast[Idx]) {
        Mismatches++;
      }
    }
    double Elapsed = OsGetTimeSeconds() - Start;
    if (Pass == 0) {
      FastSeconds = Elapsed;
    } else {
      SlowSeconds = Elapsed;
    }
  }
  free(Fast);
  LogInfo("LOD_TEST: ok lattice terrain top equals GetTerrainTop on %d/%d "
          "columns, %.2f us vs %.2f us per column",
          Count - Mismatches, Count, FastSeconds * MICROS_PER_SECOND / Count,
          SlowSeconds * MICROS_PER_SECOND / Count);
  return Check(Mismatches == 0, "lattice terrain top matches GetTerrainTop");
}

// --- determinism and cost ----------------------------------------------------

typedef struct {
  int Level;
  int X;
  int Z;
} ColumnRef;

// Column Idx of the determinism and cost pass: COLUMNS_PER_LEVEL per level
// around the spawn, level 1 first.
static ColumnRef SpawnColumn(const SpawnPoint *Spawn, int Idx) {
  static const int OFFSETS[COLUMNS_PER_LEVEL][2] = {{0, 0}, {1, 0}, {-2, 1}, {3, -3}};
  int Level = 1 + (Idx / COLUMNS_PER_LEVEL);
  int Offset = Idx % COLUMNS_PER_LEVEL;
  int Shift = 4 + Level;
  return (ColumnRef){.Level = Level,
                     .X = LodFloorShift(Spawn->X, Shift) + OFFSETS[Offset][0],
                     .Z = LodFloorShift(Spawn->Z, Shift) + OFFSETS[Offset][1]};
}

static bool TestDeterminism(LodGenScratch *Scratch, Collected *Work,
                            const SpawnPoint *Spawn, uint64_t Seed) {
  enum { COUNT = LOD_MAX_LEVEL * COLUMNS_PER_LEVEL };
  uint64_t First[COUNT];
  int Count = COUNT;
  double LevelSeconds[LOD_MAX_LEVEL + 1] = {0};
  int LevelSections[LOD_MAX_LEVEL + 1] = {0};
  int LevelSamples[LOD_MAX_LEVEL + 1] = {0};
  uint64_t Combined = FNV_OFFSET;
  for (int Idx = 0; Idx < Count; Idx++) {
    ColumnRef Column = SpawnColumn(Spawn, Idx);
    const ColumnRef *Ref = &Column;
    double Start = OsGetTimeSeconds();
    Generate(Scratch, Work, Ref->Level, Ref->X, Ref->Z, Seed);
    LevelSeconds[Ref->Level] += OsGetTimeSeconds() - Start;
    LevelSections[Ref->Level] += Work->Count;
    LevelSamples[Ref->Level] += Work->Result.TopSamples + Work->Result.BiomeSamples;
    First[Idx] = HashCollected(Work, Ref->Level, Ref->X, Ref->Z);
    Combined = HashWord(Combined, First[Idx]);
  }
  bool Same = true;
  for (int Idx = Count - 1; Idx >= 0; Idx--) {
    ColumnRef Column = SpawnColumn(Spawn, Idx);
    const ColumnRef *Ref = &Column;
    Generate(Scratch, Work, Ref->Level, Ref->X, Ref->Z, Seed);
    Same = Same && HashCollected(Work, Ref->Level, Ref->X, Ref->Z) == First[Idx];
  }
  uint64_t Other = FNV_OFFSET;
  for (int Idx = 0; Idx < Count; Idx++) {
    ColumnRef Column = SpawnColumn(Spawn, Idx);
    const ColumnRef *Ref = &Column;
    Generate(Scratch, Work, Ref->Level, Ref->X, Ref->Z, Seed ^ SEED_SALT);
    Other = HashWord(Other, HashCollected(Work, Ref->Level, Ref->X, Ref->Z));
  }
  if (!Check(Same, "generated columns repeat exactly in reverse order") ||
      !Check(Other != Combined, "another seed generates other columns")) {
    return false;
  }
  LogInfo("LOD_TEST: ok synthetic generation deterministic over %d columns "
          "(hash %016llx)",
          Count, (unsigned long long)Combined);
  for (int Level = 1; Level <= LOD_MAX_LEVEL; Level++) {
    double Micros = LevelSeconds[Level] * MICROS_PER_SECOND;
    LogInfo("LOD_TEST: ok gen cost L%d: %.0f us/column, %.0f us/section, %.1f "
            "sections/column, %d samples/column",
            Level, Micros / COLUMNS_PER_LEVEL,
            (LevelSections[Level] > 0) ? Micros / LevelSections[Level] : 0.0,
            (double)LevelSections[Level] / COLUMNS_PER_LEVEL,
            LevelSamples[Level] / COLUMNS_PER_LEVEL);
  }
  return true;
}

// --- parent / child consistency ----------------------------------------------

static uint64_t OctantMask(int Octant) {
  uint64_t Mask = 0;
  int Ox = (Octant & 1) * 2;
  int Oy = ((Octant >> 1) & 1) * 2;
  int Oz = ((Octant >> 2) & 1) * 2;
  for (int Cell = 0; Cell < OCTANTS; Cell++) {
    Mask |= LodCellBit(Ox + (Cell & 1), Oy + ((Cell >> 1) & 1), Oz + ((Cell >> 2) & 1));
  }
  return Mask;
}

static bool TestHierarchy(LodGenScratch *Scratch, Collected *Parent,
                          Collected *Children, const SpawnPoint *Spawn,
                          uint64_t Seed) {
  int Wanted = 0;
  int Found = 0;
  for (int Level = 2; Level <= LOD_MAX_LEVEL; Level++) {
    int X = LodFloorShift(Spawn->X, 4 + Level);
    int Z = LodFloorShift(Spawn->Z, 4 + Level);
    Generate(Scratch, Parent, Level, X, Z, Seed);
    for (int Child = 0; Child < 4; Child++) {
      int Cx = (X * 2) + (Child & 1);
      int Cz = (Z * 2) + (Child >> 1);
      Generate(Scratch, &Children[Child], Level - 1, Cx, Cz, Seed);
    }
    for (int Idx = 0; Idx < Parent->Count; Idx++) {
      for (int Octant = 0; Octant < OCTANTS; Octant++) {
        if ((Parent->Present[Idx] & OctantMask(Octant)) == 0U) {
          continue;
        }
        int Child = (Octant & 1) | (((Octant >> 2) & 1) << 1);
        int ChildY = (Parent->Y[Idx] * 2) + ((Octant >> 1) & 1);
        Wanted++;
        if (FindCollected(&Children[Child], ChildY) >= 0) {
          Found++;
        }
      }
    }
  }
  double Share = (Wanted > 0) ? (double)Found * PERCENT / (double)Wanted : 100.0;
  LogInfo("LOD_TEST: ok %d/%d present parent octants (%.1f%%) have their child "
          "section, levels 2-7 at spawn",
          Found, Wanted, Share);
  return Check(Share >= MIN_HIERARCHY, "present parent octants refine into children");
}

// --- agreement with real chunks ---------------------------------------------

static bool GenerateRealChunk(Chunk *Out, int ChunkX, int ChunkY, int ChunkZ,
                              uint64_t Seed, bool Features) {
  const ColumnEntry *Column = ColumnCacheLookup(
      GetColumnCache(COLUMN_CACHE_MAIN_SLOT), ChunkX, ChunkZ, Seed);
  if (Column == NULL) {
    return false;
  }
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
  if (Features) {
    PlaceChunkFeatures(Out, GetColumnCache(COLUMN_CACHE_MAIN_SLOT), true);
    PlaceChunkFlora(Out, &SLocal);
  }
  return true;
}

static void PatchOfChunk(const Chunk *Source, LodVoxel *Grid, LodPatch8 *Out) {
  for (int X = 0; X < CHUNK_SIZE; X++) {
    for (int Y = 0; Y < CHUNK_SIZE; Y++) {
      for (int Z = 0; Z < CHUNK_SIZE; Z++) {
        Grid[LodVoxelIndex(X, Y, Z)] = LodCanonicalVoxel(
            ChunkGetLocal(Source, X, Y, Z), GetChunkBiomeAtLocal(Source, X, Y, Z));
      }
    }
  }
  LodDownsampleVoxels(Grid, Out);
}

typedef struct {
  int ChunkX;
  int ChunkZ;
  int ChunkYMin;
  int ChunkYMax;
} Region;

static Region RegionAround(int BlockX, int BlockZ, uint64_t Seed) {
  int Width = REGION_CHUNKS * CHUNK_SIZE;
  Region Out = {.ChunkX = LodFloorShift(BlockX, REGION_SHIFT) * REGION_CHUNKS,
                .ChunkZ = LodFloorShift(BlockZ, REGION_SHIFT) * REGION_CHUNKS};
  int Low = INT_MAX;
  int High = SEA_LEVEL;
  for (int X = 0; X <= Width; X += 4) {
    for (int Z = 0; Z <= Width; Z += 4) {
      int Top = GetTerrainTopOnLattice((Out.ChunkX * CHUNK_SIZE) + X,
                                       (Out.ChunkZ * CHUNK_SIZE) + Z, Seed);
      Low = (Top < Low) ? Top : Low;
      High = (Top > High) ? Top : High;
    }
  }
  Out.ChunkYMin = LodFloorShift(Low - REGION_MARGIN_BLOCKS, 4);
  Out.ChunkYMax = LodFloorShift(High + REGION_MARGIN_BLOCKS, 4);
  return Out;
}

static bool IngestRegion(LodStore *Store, const Region *R, Chunk *Work,
                         LodVoxel *Grid, uint64_t Seed, bool Features) {
  LodPatch8 Patch;
  for (int Cx = R->ChunkX; Cx < R->ChunkX + REGION_CHUNKS; Cx++) {
    for (int Cz = R->ChunkZ; Cz < R->ChunkZ + REGION_CHUNKS; Cz++) {
      for (int Cy = R->ChunkYMin; Cy <= R->ChunkYMax; Cy++) {
        if (!GenerateRealChunk(Work, Cx, Cy, Cz, Seed, Features)) {
          return false;
        }
        PatchOfChunk(Work, Grid, &Patch);
        if (LodStoreWritePatchDeferred(Store, 1, Cx, Cy, Cz, &Patch,
                                       LOD_ORIGIN_INGESTED) == LOD_WRITE_NO_MEMORY) {
          return false;
        }
      }
    }
  }
  return LodStoreFlush(Store);
}

static LodVoxel RealVoxel(const LodStore *Store, int Level, int X, int Y, int Z) {
  int Sx = LodFloorShift(X, 4);
  int Sy = LodFloorShift(Y, 4);
  int Sz = LodFloorShift(Z, 4);
  const LodSection *Section = LodStoreFind(Store, Level, Sx, Sy, Sz);
  if (Section == NULL) {
    return LOD_VOXEL_AIR;
  }
  return LodSectionGet(Section, X - (Sx * LOD_SECTION_SIZE),
                       Y - (Sy * LOD_SECTION_SIZE), Z - (Sz * LOD_SECTION_SIZE));
}

static LodClass VoxelClass(LodVoxel Voxel) {
  return LodClassifyBlock(LodVoxelBlock(Voxel));
}

typedef struct {
  int64_t Columns;
  int64_t TopExact;
  int64_t TopNear;
  int64_t SurfaceSame;
  int64_t Voxels;
  int64_t VoxelsSame;
} Agreement;

static double Pct(int64_t Part, int64_t Whole) {
  return (Whole > 0) ? (double)Part * PERCENT / (double)Whole : 0.0;
}

// The highest opaque voxel of a column at or below High, or INT_MIN.
static int RealTop(const LodStore *Store, int Level, int X, int Z, int Low, int High) {
  for (int Y = High; Y >= Low; Y--) {
    if (VoxelClass(RealVoxel(Store, Level, X, Y, Z)) == LOD_CLASS_OPAQUE) {
      return Y;
    }
  }
  return INT_MIN;
}

static int SyntheticTop(const Collected *In, int I, int K, int Low, int High) {
  for (int Y = High; Y >= Low; Y--) {
    LodVoxel Voxel = LOD_VOXEL_AIR;
    if (CollectedVoxel(In, I, Y, K, &Voxel) && VoxelClass(Voxel) == LOD_CLASS_OPAQUE) {
      return Y;
    }
  }
  return INT_MIN;
}

static void CompareColumn(const LodStore *Store, const Collected *Synth, int Level,
                          int SectionX, int SectionZ, int Low, int High,
                          Agreement *Out) {
  int SeaVoxel = LodFloorShift(SEA_LEVEL, Level);
  for (int I = 0; I < LOD_SECTION_SIZE; I++) {
    for (int K = 0; K < LOD_SECTION_SIZE; K++) {
      int X = (SectionX * LOD_SECTION_SIZE) + I;
      int Z = (SectionZ * LOD_SECTION_SIZE) + K;
      int Real = RealTop(Store, Level, X, Z, Low, High);
      int Syn = SyntheticTop(Synth, I, K, Low, High);
      if (Real == INT_MIN || Syn == INT_MIN || Real == High) {
        continue;
      }
      Out->Columns++;
      int Diff = Real - Syn;
      Out->TopExact += (Diff == 0) ? 1 : 0;
      Out->TopNear += (Diff >= -1 && Diff <= 1) ? 1 : 0;
      if (Diff == 0) {
        LodVoxel SynVoxel = LOD_VOXEL_AIR;
        (void)CollectedVoxel(Synth, I, Syn, K, &SynVoxel);
        Out->SurfaceSame +=
            (LodVoxelBlock(RealVoxel(Store, Level, X, Real, Z)) == LodVoxelBlock(SynVoxel))
                ? 1
                : 0;
      }
      int BandLow = ((Real < Syn) ? Real : Syn) - BAND_VOXELS;
      int BandHigh = ((Real > Syn) ? Real : Syn);
      BandHigh = ((BandHigh > SeaVoxel) ? BandHigh : SeaVoxel) + BAND_VOXELS;
      BandLow = (BandLow > Low) ? BandLow : Low;
      BandHigh = (BandHigh < High) ? BandHigh : High;
      for (int Y = BandLow; Y <= BandHigh; Y++) {
        LodVoxel SynVoxel = LOD_VOXEL_AIR;
        // The voxel holding the sea level row differs by design: the
        // downsample makes it water, the generator keeps every level's sea
        // surface at the sea level (see lod_gen.h).
        if (Y == SeaVoxel || !CollectedVoxel(Synth, I, Y, K, &SynVoxel)) {
          continue;
        }
        Out->Voxels++;
        Out->VoxelsSame +=
            (VoxelClass(RealVoxel(Store, Level, X, Y, Z)) == VoxelClass(SynVoxel)) ? 1 : 0;
      }
    }
  }
}

static void CompareRegion(const LodStore *Store, LodGenScratch *Scratch,
                          Collected *Synth, const Region *R, uint64_t Seed,
                          Agreement Out[AGREEMENT_LEVELS + 1]) {
  for (int Level = 1; Level <= AGREEMENT_LEVELS; Level++) {
    int Step = 1 << Level;
    // Voxels wholly inside the ingested chunk rows, one voxel in from each end
    // so the downsample's exposure test saw real neighbours.
    int Low = LodFloorShift(R->ChunkYMin * CHUNK_SIZE, Level) + 1;
    int High = LodFloorShift((R->ChunkYMax * CHUNK_SIZE) + CHUNK_SIZE - Step, Level) - 1;
    int Columns = REGION_CHUNKS >> Level;
    int BaseX = LodFloorShift(R->ChunkX, Level);
    int BaseZ = LodFloorShift(R->ChunkZ, Level);
    for (int A = 0; A < Columns; A++) {
      for (int B = 0; B < Columns; B++) {
        Generate(Scratch, Synth, Level, BaseX + A, BaseZ + B, Seed);
        CompareColumn(Store, Synth, Level, BaseX + A, BaseZ + B, Low, High,
                      &Out[Level]);
      }
    }
  }
}

static bool MeasureAgreement(LodGenScratch *Scratch, Collected *Synth,
                             const SpawnPoint *Spawn, uint64_t Seed, bool Features,
                             Agreement Out[AGREEMENT_LEVELS + 1]) {
  Chunk *Work = ChunkCreateStandalone();
  LodVoxel *Grid = (LodVoxel *)malloc(sizeof(LodVoxel) * LOD_SECTION_VOLUME);
  bool Passed = Check(Work != NULL && Grid != NULL, "agreement scratch allocation");
  static const int OFFSETS[REGION_COUNT][2] = {
      {0, 0}, {REGION_OFFSET, 0}, {0, -REGION_OFFSET}};
  for (int Idx = 0; Passed && Idx < REGION_COUNT; Idx++) {
    Region R = RegionAround(Spawn->X + OFFSETS[Idx][0], Spawn->Z + OFFSETS[Idx][1], Seed);
    LodStoreConfig Config;
    LodStoreDefaultConfig(&Config);
    LodStore Store;
    Passed = Check(LodStoreInit(&Store, &Config), "agreement store allocation");
    if (!Passed) {
      break;
    }
    Passed = Check(IngestRegion(&Store, &R, Work, Grid, Seed, Features),
                   "agreement region ingestion");
    if (Passed) {
      CompareRegion(&Store, Scratch, Synth, &R, Seed, Out);
    }
    LodStoreFree(&Store);
  }
  free(Grid);
  if (Work != NULL) {
    ChunkFreeStandalone(Work);
  }
  return Passed;
}

static bool TestAgreement(LodGenScratch *Scratch, Collected *Synth,
                          const SpawnPoint *Spawn, uint64_t Seed) {
  Agreement Terrain[AGREEMENT_LEVELS + 1] = {0};
  Agreement Features[AGREEMENT_LEVELS + 1] = {0};
  if (!MeasureAgreement(Scratch, Synth, Spawn, Seed, false, Terrain) ||
      !MeasureAgreement(Scratch, Synth, Spawn, Seed, true, Features)) {
    return false;
  }
  bool Passed = true;
  for (int Level = 1; Level <= AGREEMENT_LEVELS; Level++) {
    const Agreement *T = &Terrain[Level];
    const Agreement *F = &Features[Level];
    double Occupancy = Pct(T->VoxelsSame, T->Voxels);
    double Exact = Pct(T->TopExact, T->Columns);
    double Near = Pct(T->TopNear, T->Columns);
    double Surface = Pct(T->SurfaceSame, T->TopExact);
    LogInfo("LOD_TEST: ok agreement L%d vs real terrain: occupancy %.1f%% of %lld "
            "voxels, top exact %.1f%% / +-1 %.1f%% of %lld columns, surface block "
            "%.1f%% (minimums %.0f/%.0f/%.0f/%.0f)",
            Level, Occupancy, (long long)T->Voxels, Exact, Near, (long long)T->Columns,
            Surface, MIN_OCCUPANCY[Level], MIN_TOP_EXACT[Level], MIN_TOP_NEAR[Level],
            MIN_SURFACE[Level]);
    LogInfo("LOD_TEST: ok agreement L%d vs real chunks with trees and flora: "
            "occupancy %.1f%%, top exact %.1f%% / +-1 %.1f%%, surface block %.1f%%",
            Level, Pct(F->VoxelsSame, F->Voxels), Pct(F->TopExact, F->Columns),
            Pct(F->TopNear, F->Columns), Pct(F->SurfaceSame, F->TopExact));
    char What[CHECK_TEXT_CHARS];
    (void)snprintf(What, sizeof(What), "level %d agreement above its minimums", Level);
    Passed = Check(Occupancy >= MIN_OCCUPANCY[Level] && Exact >= MIN_TOP_EXACT[Level] &&
                       Near >= MIN_TOP_NEAR[Level] && Surface >= MIN_SURFACE[Level],
                   What) &&
             Passed;
  }
  return Passed;
}

// --- precedence ---------------------------------------------------------------

static bool EncodeFilled(LodSection *Out, LodVoxel Value, LodVoxel *Grid) {
  for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
    Grid[Idx] = Value;
  }
  // A second value keeps it from being uniform, so the adopt path is taken.
  Grid[LOD_SECTION_VOLUME - 1] = LOD_VOXEL_AIR;
  LodSectionInit(Out);
  return LodSectionEncode(Out, Grid);
}

static bool TestSyntheticPrecedence(const SpawnPoint *Spawn, uint64_t Seed) {
  Chunk *Work = ChunkCreateStandalone();
  LodVoxel *Grid = (LodVoxel *)malloc(sizeof(LodVoxel) * LOD_SECTION_VOLUME);
  LodVoxel *Before = (LodVoxel *)malloc(sizeof(LodVoxel) * LOD_SECTION_VOLUME);
  LodStoreConfig Config;
  LodStoreDefaultConfig(&Config);
  LodStore Store;
  bool Passed = Check(Work != NULL && Grid != NULL && Before != NULL &&
                          LodStoreInit(&Store, &Config),
                      "precedence scratch allocation");
  if (!Passed) {
    free(Grid);
    free(Before);
    if (Work != NULL) {
      ChunkFreeStandalone(Work);
    }
    return false;
  }
  LodPatch8 Real;
  Passed = GenerateRealChunk(Work, LodFloorShift(Spawn->X, 4),
                             LodFloorShift(Spawn->Surface, 4),
                             LodFloorShift(Spawn->Z, 4), Seed, true);
  PatchOfChunk(Work, Grid, &Real);
  BlockID Stone = TerrainStoneBlockId();
  LodVoxel StoneVoxel = LodCanonicalVoxel(Stone, 0);

  // One real chunk in octant 0 of the level-1 section (0, 0, 0).
  Passed = Passed && Check(LodStoreWritePatch(&Store, 1, 0, 0, 0, &Real,
                                              LOD_ORIGIN_INGESTED) == LOD_WRITE_APPLIED,
                           "real chunk written");
  const LodSection *Level1 = LodStoreFind(&Store, 1, 0, 0, 0);
  Passed = Passed && Check(Level1 != NULL, "level-1 section exists");
  uint64_t Ingested = (Level1 != NULL) ? Level1->IngestedCells : 0U;
  if (Passed) {
    LodSectionDecode(Level1, Before);
  }
  LodSection Encoded;
  Passed = Passed && Check(EncodeFilled(&Encoded, StoneVoxel, Grid), "synthetic encode");
  int Pending = Store.PendingCount;
  int Hot = Store.HotLive;
  Passed = Passed &&
           Check(LodStoreWriteSynthetic(&Store, 1, 0, 0, 0, &Encoded, ~(uint64_t)0U) ==
                     LOD_WRITE_APPLIED,
                 "synthetic section over partly real data applies") &&
           Check(Level1->IngestedCells == Ingested && Ingested != 0U,
                 "real cells keep their provenance") &&
           Check(Level1->PresentCells == ~(uint64_t)0U, "synthetic cells present") &&
           Check(Store.PendingCount == Pending && Store.HotLive == Hot,
                 "a synthetic section neither propagates nor turns hot");
  bool RealKept = true;
  bool SynthLanded = true;
  for (int X = 0; Passed && X < LOD_SECTION_SIZE; X++) {
    for (int Y = 0; Y < LOD_SECTION_SIZE; Y++) {
      for (int Z = 0; Z < LOD_SECTION_SIZE; Z++) {
        bool InReal = X < LOD_PATCH_SIZE && Y < LOD_PATCH_SIZE && Z < LOD_PATCH_SIZE;
        LodVoxel Now = LodSectionGet(Level1, X, Y, Z);
        if (InReal) {
          RealKept = RealKept && Now == Before[LodVoxelIndex(X, Y, Z)];
        } else if (LodVoxelIndex(X, Y, Z) != LOD_SECTION_VOLUME - 1) {
          SynthLanded = SynthLanded && Now == StoneVoxel;
        }
      }
    }
  }
  Passed = Passed && Check(RealKept, "voxels of real cells unchanged") &&
           Check(SynthLanded, "voxels of the other cells are synthetic");
  LodSectionFree(&Encoded);

  // Every octant real: a synthetic section changes nothing.
  for (int Octant = 1; Passed && Octant < OCTANTS; Octant++) {
    Passed = LodStoreWritePatch(&Store, 1, Octant & 1, (Octant >> 1) & 1,
                                (Octant >> 2) & 1, &Real,
                                LOD_ORIGIN_INGESTED) == LOD_WRITE_APPLIED;
  }
  uint64_t Hash = LodStoreLevelHash(&Store, 1);
  Passed = Passed && Check(EncodeFilled(&Encoded, StoneVoxel, Grid), "synthetic encode") &&
           Check(LodStoreWriteSynthetic(&Store, 1, 0, 0, 0, &Encoded, ~(uint64_t)0U) ==
                     LOD_WRITE_SKIPPED_PRECEDENCE,
                 "synthetic section over real data is skipped") &&
           Check(LodStoreLevelHash(&Store, 1) == Hash, "skipped section changes nothing");
  LodSectionFree(&Encoded);

  // A synthetic-only section, then a real chunk into it: real wins.
  Passed = Passed && Check(EncodeFilled(&Encoded, StoneVoxel, Grid), "synthetic encode") &&
           Check(LodStoreWriteSynthetic(&Store, 1, SYNTH_SECTION, 0, SYNTH_SECTION,
                                        &Encoded, ~(uint64_t)0U) ==
                     LOD_WRITE_APPLIED,
                 "synthetic section into an empty slot applies") &&
           Check(LodStoreWritePatch(&Store, 1, SYNTH_CHUNK, 0, SYNTH_CHUNK, &Real,
                                    LOD_ORIGIN_INGESTED) ==
                     LOD_WRITE_APPLIED,
                 "real chunk over a synthetic section applies");
  const LodSection *Mixed = LodStoreFind(&Store, 1, SYNTH_SECTION, 0, SYNTH_SECTION);
  Passed = Passed && Check(Mixed != NULL && LodSectionGet(Mixed, 0, 0, 0) ==
                                                Real.Voxels[LodPatchIndex(0, 0, 0)],
                           "real data replaces synthetic data");
  LodSectionFree(&Encoded);
  LodStoreFree(&Store);
  free(Grid);
  free(Before);
  ChunkFreeStandalone(Work);
  if (Passed) {
    LogInfo("LOD_TEST: ok synthetic sections never replace real cells and real "
            "chunks replace synthetic ones");
  }
  return Passed;
}

bool RunLodGenSelfTest(void) {
  uint64_t Seed = GetWorldSeed();
  SpawnPoint Spawn = FindSpawnColumn(Seed);
  LodGenScratch *Scratch = LodGenScratchCreate();
  Collected Work;
  Collected Children[4];
  bool Ready = Scratch != NULL && CollectedInit(&Work);
  for (int Idx = 0; Idx < 4; Idx++) {
    Ready = CollectedInit(&Children[Idx]) && Ready;
  }
  bool Passed = Check(Ready, "generator scratch allocation") &&
                TestLatticeTop(Seed, Spawn.X, Spawn.Z) &&
                TestDeterminism(Scratch, &Work, &Spawn, Seed) &&
                TestHierarchy(Scratch, &Work, Children, &Spawn, Seed) &&
                TestSyntheticPrecedence(&Spawn, Seed) &&
                TestAgreement(Scratch, &Work, &Spawn, Seed);
  free(Work.Voxels);
  for (int Idx = 0; Idx < 4; Idx++) {
    free(Children[Idx].Voxels);
  }
  LodGenScratchFree(Scratch);
  return Passed;
}
