#include "world/feature.h"
#include "core/hash.h"
#include "core/utils.h"
#include "world/chunk.h"
#include "world/surface.h"
#include "world/terrain_density.h"
#include "world/worldgen_column_cache.h"
#include "world/biome.h"
#include "world/prefab.h"
#include "persistence/world_save.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
  // World space is tiled into GRID_CELL_SIZE^2 columns; each cell yields at most
  // one structure candidate, which both spaces structures apart and bounds how
  // many cells a chunk's scan has to visit.
  GRID_CELL_SIZE = 6
};

// Salts keep the structure grid statistically independent of the flora rolls
// and of each other, so a column's existence roll and its prefab roll do not
// correlate.
#define STRUCTURE_EXIST_SALT 0x57A0C7DE00000001ULL
#define STRUCTURE_KIND_SALT 0x57A0C7DE00000002ULL
#define STRUCTURE_ORIENT_SALT 0x57A0C7DE00000003ULL

// Fold the seed and both cell coordinates into one 64-bit value whose disjoint
// bit fields drive existence and the in-cell jitter.
static uint64_t HashCell(int CellX, int CellZ, uint64_t Seed) {
  const uint64_t PRIME_X = 0x9E3779B97F4A7C15ULL;
  const uint64_t PRIME_Z = 0xC2B2AE3D27D4EB4FULL;

  uint64_t Hash = Seed;
  Hash = SplitMix64(Hash ^ ((uint64_t)(uint32_t)CellX * PRIME_X));
  Hash = SplitMix64(Hash ^ ((uint64_t)(uint32_t)CellZ * PRIME_Z));
  return Hash;
}

// Floor division toward negative infinity, so the grid tiles continuously across
// the origin instead of mirroring around it.
static int FloorDiv(int Numerator, int Denominator) {
  int Quotient = Numerator / Denominator;
  int Remainder = Numerator % Denominator;
  if (Remainder != 0 && ((Remainder < 0) != (Denominator < 0))) {
    Quotient--;
  }
  return Quotient;
}

// One candidate per grid cell: the cell's hash picks a jittered column so
// structures never land on a fixed lattice. Existence and prefab choice are
// decided by the column's biome, not here.
static void CellCandidate(int CellX, int CellZ, uint64_t Seed, int *OutX,
                          int *OutZ) {
  const unsigned int SHIFT_JITTER_X = 16U;
  const unsigned int SHIFT_JITTER_Z = 32U;

  uint64_t Hash = HashCell(CellX, CellZ, Seed);
  int JitterX = (int)((Hash >> SHIFT_JITTER_X) % (uint64_t)GRID_CELL_SIZE);
  int JitterZ = (int)((Hash >> SHIFT_JITTER_Z) % (uint64_t)GRID_CELL_SIZE);
  *OutX = (CellX * GRID_CELL_SIZE) + JitterX;
  *OutZ = (CellZ * GRID_CELL_SIZE) + JitterZ;
}

// Pick a structure prefab index from the biome's weighted set using a hash draw.
// Assumes StructureCount > 0 and StructureTotalWeight > 0.
static int PickStructurePrefab(const BiomeType *Biome, uint64_t Hash) {
  int Count = Biome->StructureCount;
  if (Count > MAX_BIOME_STRUCTURES) {
    Count = MAX_BIOME_STRUCTURES;
  }
  int Roll = (int)(Hash % (uint64_t)Biome->StructureTotalWeight);
  for (int Idx = 0; Idx < Count; Idx++) {
    Roll -= Biome->Structures[Idx].Weight;
    if (Roll < 0) {
      return Biome->Structures[Idx].PrefabIndex;
    }
  }
  return Biome->Structures[0].PrefabIndex;
}

// Draw a 0-7 orientation code from a hash, then clamp it to the orientations
// the prefab permits: a prefab that forbids rotation keeps the low two bits at
// 0, and one that forbids mirroring clears the mirror bit. Masking after the
// draw keeps the roll independent of prefab identity.
static int PickOrientation(const Prefab *PrefabVal, uint64_t Hash) {
  const int MIRROR_BIT = 0x4;
  const int ROTATION_MASK = 0x3;
  const uint64_t ORIENTATION_COUNT = 8ULL;

  int Code = (int)(Hash % ORIENTATION_COUNT);
  if (PrefabVal->AllowRotation == false) {
    Code &= ~ROTATION_MASK;
  }
  if (PrefabVal->AllowMirror == false) {
    Code &= ~MIRROR_BIT;
  }
  return Code;
}

// Reach of every loaded prefab around its anchor column: horizontally the widest
// footprint, vertically how far it extends below and above the cell it is
// stamped at. A candidate's prefab is unknown until its biome resolves, so these
// are upper bounds over all prefabs.
typedef struct {
  int Footprint;
  int Below;
  int Above;
} PrefabReach;

static PrefabReach MaxPrefabReach(void) {
  PrefabReach Reach = {0, 0, 0};
  int Count = GetLoadedPrefabsCount();
  for (int Idx = 0; Idx < Count; Idx++) {
    const Prefab *PrefabVal = GetPrefabByIndex(Idx);
    if (PrefabVal == NULL) {
      continue;
    }
    int Footprint = PrefabVal->SizeX > PrefabVal->SizeZ ? PrefabVal->SizeX
                                                        : PrefabVal->SizeZ;
    int AnchorY = PrefabVal->HasOrigin ? PrefabVal->OriginY : 0;
    int Above = PrefabVal->SizeY - 1 - AnchorY;
    Reach.Footprint = (Footprint > Reach.Footprint) ? Footprint : Reach.Footprint;
    Reach.Below = (AnchorY > Reach.Below) ? AnchorY : Reach.Below;
    Reach.Above = (Above > Reach.Above) ? Above : Reach.Above;
  }
  return Reach;
}

// Preliminary surface of any column, read from its cache entry. The entry holds
// exactly what GetPreliminarySurface returns, so this is the same value without
// re-evaluating the terrain shape. The entry pointer is used immediately: the
// next lookup may reuse its slot.
static int CachedSurface(ColumnCache *Cache, int WorldX, int WorldZ,
                         uint64_t Seed) {
  int ColumnX = FloorDiv(WorldX, CHUNK_SIZE);
  int ColumnZ = FloorDiv(WorldZ, CHUNK_SIZE);
  const ColumnEntry *Entry = ColumnCacheLookup(Cache, ColumnX, ColumnZ, Seed);
  return Entry->Surface[WorldX - (ColumnX * CHUNK_SIZE)]
                       [WorldZ - (ColumnZ * CHUNK_SIZE)];
}

// The biome SampleBiomeAt gives at (WorldX, Y, WorldZ), read from the column's
// cache entry: the cell column's cached surface biome when the cell lies in the
// surface depth band, otherwise a resolution of its cached climate. Either way
// the same id as the pure sampler, without re-evaluating the terrain shape.
static int CachedBiome(ColumnCache *Cache, int WorldX, int Y, int WorldZ,
                       uint64_t Seed) {
  int ColumnX = FloorDiv(WorldX, CHUNK_SIZE);
  int ColumnZ = FloorDiv(WorldZ, CHUNK_SIZE);
  const ColumnEntry *Entry = ColumnCacheLookup(Cache, ColumnX, ColumnZ, Seed);
  int CellX = (WorldX - (ColumnX * CHUNK_SIZE)) / BIOME_CELL_SIZE;
  int CellZ = (WorldZ - (ColumnZ * CHUNK_SIZE)) / BIOME_CELL_SIZE;
  int Depth = Entry->CellFloor[CellX][CellZ] - BiomeCellCenter(Y);
  if (BiomeInSurfaceBand(Depth)) {
    return Entry->SurfaceBiome[CellX][CellZ];
  }
  BiomeClimate Climate = Entry->Climate[CellX][CellZ];
  Climate.Depth = (float)Depth;
  return ResolveBiome(&Climate);
}

// True when no structure anchored anywhere in the scanned column range can
// reach this cube: every anchor is a terrain top, and no solid block (so no
// top) lies above a column's upper density bound. Below the surface there is no
// cheap bound on a top, so only cubes above every candidate are skipped.
static bool CubeOutOfReach(const Chunk *ChunkVal, ColumnCache *Cache,
                           int MinX, int MaxX, int MinZ, int MaxZ,
                           const PrefabReach *Reach, uint64_t Seed) {
  float HighestBound = 0.0F;
  bool First = true;
  for (int ColumnX = FloorDiv(MinX, CHUNK_SIZE);
       ColumnX <= FloorDiv(MaxX, CHUNK_SIZE); ColumnX++) {
    for (int ColumnZ = FloorDiv(MinZ, CHUNK_SIZE);
         ColumnZ <= FloorDiv(MaxZ, CHUNK_SIZE); ColumnZ++) {
      const ColumnEntry *Entry =
          ColumnCacheLookup(Cache, ColumnX, ColumnZ, Seed);
      HighestBound = (First || Entry->BoundHigh > HighestBound)
                         ? Entry->BoundHigh
                         : HighestBound;
      First = false;
    }
  }

  // A structure is stamped from T + 1 - Below to T + 1 + Above.
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  int ReachHigh = (int)floorf(HighestBound) + 1 + Reach->Above;
  return BaseY > ReachHigh;
}

// Terrain tops of recent candidate columns, per thread. GetTerrainTop is pure,
// so a hit is exactly a fresh probe; a worker generating a column's vertical
// stack, and its neighbours, keeps meeting the same candidates.
enum {
  TOP_MEMO_SLOTS = 2048
};

typedef struct {
  int X;
  int Z;
  uint64_t Seed;
  bool Valid;
  int Top;
  int SolidBelow;
} TopMemoEntry;

// Shattered terrain leaves thin floating fragments; a structure needs ground
// at least GROUND_THICKNESS blocks deep under its anchor.
enum {
  GROUND_THICKNESS = 3
};

static int CachedTerrainTop(int WorldX, int WorldZ, uint64_t Seed,
                            int *OutSolidBelow) {
  static _Thread_local TopMemoEntry SMemo[TOP_MEMO_SLOTS];
  const unsigned int HALF_BITS = 32U;
  uint64_t Key = ((uint64_t)(uint32_t)WorldX << HALF_BITS) |
                 (uint64_t)(uint32_t)WorldZ;
  TopMemoEntry *Entry =
      &SMemo[SplitMix64(Key ^ Seed) % (uint64_t)TOP_MEMO_SLOTS];
  if (Entry->Valid && Entry->X == WorldX && Entry->Z == WorldZ &&
      Entry->Seed == Seed) {
    if (OutSolidBelow != NULL) {
      *OutSolidBelow = Entry->SolidBelow;
    }
    return Entry->Top;
  }
  int SolidBelow = 0;
  int Top = GetTerrainTopWithDepth(WorldX, WorldZ, Seed, GROUND_THICKNESS - 1,
                                   &SolidBelow);
  *Entry = (TopMemoEntry){.X = WorldX,
                          .Z = WorldZ,
                          .Seed = Seed,
                          .Valid = true,
                          .Top = Top,
                          .SolidBelow = SolidBelow};
  if (OutSolidBelow != NULL) {
    *OutSolidBelow = SolidBelow;
  }
  return Top;
}

typedef struct {
  int WorldX;
  int WorldZ;
  int Surface;
  int BiomeId;
  const Prefab *Structure;
} StructureCandidate;

// Everything that decides a grid cell's structure except whether it reaches a
// given cube. Cache, when given, only avoids recomputation: the pure path gives
// the same candidate.
static bool ResolveCandidateFresh(int CellX, int CellZ, uint64_t Seed,
                                 ColumnCache *Cache, StructureCandidate *Out) {
  const uint64_t MASK16 = 0xFFFFULL;
  const float DENSITY_SCALE = 65536.0F;

  CellCandidate(CellX, CellZ, Seed, &Out->WorldX, &Out->WorldZ);
  int WorldX = Out->WorldX;
  int WorldZ = Out->WorldZ;

  // The column's surface biome (the biome at its preliminary surface) decides
  // the prefab, resolved exactly as the pure sampler does, so every chunk
  // re-deriving an overhanging structure agrees on it. The existence roll
  // comes before the terrain-top probe, which only surviving candidates pay.
  int Preliminary = (Cache != NULL)
                        ? CachedSurface(Cache, WorldX, WorldZ, Seed)
                        : GetPreliminarySurface(WorldX, WorldZ, Seed);
  int BiomeId = (Cache != NULL)
                    ? CachedBiome(Cache, WorldX, Preliminary, WorldZ, Seed)
                    : SampleBiomeAt(WorldX, Preliminary, WorldZ, Seed, NULL);
  const BiomeType *Biome = GetBiomeDef(BiomeId);
  Out->BiomeId = BiomeId;
  if (Biome->StructureCount == 0 || Biome->StructureTotalWeight <= 0) {
    return false;
  }

  uint64_t ExistHash = HashCell(WorldX, WorldZ, Seed ^ STRUCTURE_EXIST_SALT);
  float Roll = (float)(ExistHash & MASK16) / DENSITY_SCALE;
  if (Roll >= Biome->StructureDensity) {
    return false;
  }

  // The anchor is the real terrain top: solid by definition with open space
  // above it, wherever the 3D field put the ground.
  int SolidBelow = 0;
  Out->Surface = CachedTerrainTop(WorldX, WorldZ, Seed, &SolidBelow);
  if (Out->Surface <= SEA_LEVEL || SolidBelow < GROUND_THICKNESS - 1) {
    return false;
  }

  uint64_t KindHash = HashCell(WorldX, WorldZ, Seed ^ STRUCTURE_KIND_SALT);
  Out->Structure = GetPrefabByIndex(PickStructurePrefab(Biome, KindHash));
  return Out->Structure != NULL;
}

static bool ResolveCandidate(int CellX, int CellZ, uint64_t Seed,
                             ColumnCache *Cache, StructureCandidate *Out);

void PlaceChunkFeatures(Chunk *ChunkVal, ColumnCache *Cache, bool AllowSkip) {
  uint64_t Seed = GetWorldSeed();

  // A structure rooted up to the largest prefab's footprint outside the chunk
  // can still stamp cells into it, so the scan widens the chunk's column range
  // by that reach on every side before mapping to grid cells.
  PrefabReach Reach = MaxPrefabReach();
  int Margin = Reach.Footprint;

  int MinX = (ChunkVal->ChunkX * CHUNK_SIZE) - Margin;
  int MaxX = (ChunkVal->ChunkX * CHUNK_SIZE) + CHUNK_SIZE - 1 + Margin;
  int MinZ = (ChunkVal->ChunkZ * CHUNK_SIZE) - Margin;
  int MaxZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + CHUNK_SIZE - 1 + Margin;

  if (AllowSkip && CubeOutOfReach(ChunkVal, Cache, MinX, MaxX, MinZ, MaxZ,
                                  &Reach, Seed)) {
    return;
  }

  int CellMinX = FloorDiv(MinX, GRID_CELL_SIZE);
  int CellMaxX = FloorDiv(MaxX, GRID_CELL_SIZE);
  int CellMinZ = FloorDiv(MinZ, GRID_CELL_SIZE);
  int CellMaxZ = FloorDiv(MaxZ, GRID_CELL_SIZE);
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  int TopY = BaseY + CHUNK_SIZE - 1;

  for (int CellX = CellMinX; CellX <= CellMaxX; CellX++) {
    for (int CellZ = CellMinZ; CellZ <= CellMaxZ; CellZ++) {
      StructureCandidate Candidate;
      if (!ResolveCandidate(CellX, CellZ, Seed, Cache, &Candidate)) {
        continue;
      }

      // Nothing to stamp here when the structure's vertical extent misses this
      // cube.
      const Prefab *Structure = Candidate.Structure;
      int AnchorY = Structure->HasOrigin ? Structure->OriginY : 0;
      int StampLow = Candidate.Surface + 1 - AnchorY;
      int StampHigh = StampLow + Structure->SizeY - 1;
      if (StampHigh < BaseY || StampLow > TopY) {
        continue;
      }

      // Orientation rolls on its own salt so it stays independent of the
      // existence and kind draws, then is masked by the prefab's flags. Keyed on
      // the anchor column, every chunk re-deriving an overhanging structure
      // agrees on its orientation.
      uint64_t OrientHash = HashCell(Candidate.WorldX, Candidate.WorldZ,
                                     Seed ^ STRUCTURE_ORIENT_SALT);
      int Orientation = PickOrientation(Structure, OrientHash);

      StampPrefabIntoChunk(ChunkVal, Structure, Candidate.WorldX,
                           Candidate.Surface + 1, Candidate.WorldZ,
                           Orientation);
    }
  }
}

// Whole-candidate memo, per thread: a cube re-derives every grid cell its
// footprint margin reaches, and the cubes of a column stack and its neighbours
// meet the same cells over and over. The resolution is pure (Cache only avoids
// recomputation), so a hit returns exactly what a fresh resolution would.
enum {
  CANDIDATE_MEMO_SLOTS = 4096
};

typedef struct {
  int CellX;
  int CellZ;
  uint64_t Seed;
  bool Valid;
  bool Placed;
  StructureCandidate Candidate;
} CandidateMemoEntry;

static bool ResolveCandidate(int CellX, int CellZ, uint64_t Seed,
                             ColumnCache *Cache, StructureCandidate *Out) {
  static _Thread_local CandidateMemoEntry SMemo[CANDIDATE_MEMO_SLOTS];
  const unsigned int HALF_BITS = 32U;
  uint64_t Key = ((uint64_t)(uint32_t)CellX << HALF_BITS) |
                 (uint64_t)(uint32_t)CellZ;
  CandidateMemoEntry *Entry =
      &SMemo[SplitMix64(Key ^ Seed) % (uint64_t)CANDIDATE_MEMO_SLOTS];
  if (!Entry->Valid || Entry->CellX != CellX || Entry->CellZ != CellZ ||
      Entry->Seed != Seed) {
    Entry->Placed =
        ResolveCandidateFresh(CellX, CellZ, Seed, Cache, &Entry->Candidate);
    Entry->CellX = CellX;
    Entry->CellZ = CellZ;
    Entry->Seed = Seed;
    Entry->Valid = true;
  }
  *Out = Entry->Candidate;
  return Entry->Placed;
}

bool ResolveStructureAt(int WorldX, int WorldZ, uint64_t Seed, int *OutBiomeId,
                        const char **OutPrefabName) {
  StructureCandidate Candidate;
  if (!ResolveCandidate(FloorDiv(WorldX, GRID_CELL_SIZE),
                        FloorDiv(WorldZ, GRID_CELL_SIZE), Seed, NULL,
                        &Candidate)) {
    return false;
  }
  *OutBiomeId = Candidate.BiomeId;
  *OutPrefabName = Candidate.Structure->Name;
  return true;
}

int FeatureTerrainTop(int WorldX, int WorldZ, uint64_t Seed) {
  return CachedTerrainTop(WorldX, WorldZ, Seed, NULL);
}

bool FindDisplacedStructureAnchor(int CellRadius, int MinGap, uint64_t Seed,
                                  int *OutX, int *OutY, int *OutZ) {
  for (int CellX = -CellRadius; CellX <= CellRadius; CellX++) {
    for (int CellZ = -CellRadius; CellZ <= CellRadius; CellZ++) {
      StructureCandidate Candidate;
      if (!ResolveCandidate(CellX, CellZ, Seed, NULL, &Candidate)) {
        continue;
      }
      int Preliminary =
          GetPreliminarySurface(Candidate.WorldX, Candidate.WorldZ, Seed);
      if (abs(Candidate.Surface - Preliminary) < MinGap) {
        continue;
      }
      *OutX = Candidate.WorldX;
      *OutY = Candidate.Surface + 1;
      *OutZ = Candidate.WorldZ;
      return true;
    }
  }
  return false;
}

bool FindStructureAnchor(int CellRadius, uint64_t Seed, int *OutX, int *OutY,
                         int *OutZ) {
  for (int CellX = -CellRadius; CellX <= CellRadius; CellX++) {
    for (int CellZ = -CellRadius; CellZ <= CellRadius; CellZ++) {
      StructureCandidate Candidate;
      if (!ResolveCandidate(CellX, CellZ, Seed, NULL, &Candidate)) {
        continue;
      }
      *OutX = Candidate.WorldX;
      *OutY = Candidate.Surface + 1;
      *OutZ = Candidate.WorldZ;
      return true;
    }
  }
  return false;
}

bool FindStructureByName(const char *PrefabName, int StartX, int StartZ,
                         int MaxRadius, uint64_t Seed, int *OutX, int *OutY,
                         int *OutZ, float *OutDistance) {
  if (PrefabName == NULL || PrefabName[0] == '\0') {
    return false;
  }

  int TargetPrefabIdx = -1;
  const Prefab *TargetPrefab = NULL;

  int LoadedPrefabs = GetLoadedPrefabsCount();
  for (int Idx = 0; Idx < LoadedPrefabs; Idx++) {
    const Prefab *P = GetPrefabByIndex(Idx);
    if (P != NULL && NameMatchesExact(P->Name, PrefabName)) {
      TargetPrefabIdx = Idx;
      TargetPrefab = P;
      break;
    }
  }
  if (TargetPrefabIdx == -1) {
    for (int Idx = 0; Idx < LoadedPrefabs; Idx++) {
      const Prefab *P = GetPrefabByIndex(Idx);
      if (P != NULL && NameMatchesPrefix(P->Name, PrefabName)) {
        TargetPrefabIdx = Idx;
        TargetPrefab = P;
        break;
      }
    }
  }

  if (TargetPrefab == NULL || TargetPrefabIdx == -1) {
    return false;
  }

  bool SpawnsInAnyBiome = false;
  int LoadedBiomes = GetLoadedBiomesCount();
  for (int BIdx = 0; BIdx < LoadedBiomes; BIdx++) {
    const BiomeType *B = GetBiomeDef(BIdx);
    if (B == NULL) {
      continue;
    }
    for (int SIdx = 0; SIdx < B->StructureCount; SIdx++) {
      if (B->Structures[SIdx].PrefabIndex == TargetPrefabIdx) {
        SpawnsInAnyBiome = true;
        break;
      }
    }
    if (SpawnsInAnyBiome) {
      break;
    }
  }

  if (!SpawnsInAnyBiome) {
    return false;
  }

  int CenterCellX = FloorDiv(StartX, GRID_CELL_SIZE);
  int CenterCellZ = FloorDiv(StartZ, GRID_CELL_SIZE);
  int MaxCellRadius = MaxRadius / GRID_CELL_SIZE;
  if (MaxCellRadius < 1) {
    MaxCellRadius = 1;
  }

  const int MAX_EVALUATIONS = 50000;
  int Evaluations = 0;

  const float INITIAL_SEARCH_DISTANCE = 1.0e9F;
  bool Found = false;
  float BestDist = INITIAL_SEARCH_DISTANCE;
  int BestX = 0;
  int BestY = 0;
  int BestZ = 0;

  const uint64_t MASK16 = 0xFFFFULL;
  const float DENSITY_SCALE = 65536.0F;

  for (int Ring = 0; Ring <= MaxCellRadius; Ring++) {
    if (Ring == 0) {
      int CandX = 0;
      int CandZ = 0;
      CellCandidate(CenterCellX, CenterCellZ, Seed, &CandX, &CandZ);
      int Prelim = GetPreliminarySurface(CandX, CandZ, Seed);
      int BId = SampleBiomeAt(CandX, Prelim, CandZ, Seed, NULL);
      const BiomeType *BiomeDef = GetBiomeDef(BId);
      if (BiomeDef != NULL && BiomeDef->StructureCount > 0 &&
          BiomeDef->StructureTotalWeight > 0) {
        bool HasPrefab = false;
        for (int StructIdx = 0; StructIdx < BiomeDef->StructureCount; StructIdx++) {
          if (BiomeDef->Structures[StructIdx].PrefabIndex == TargetPrefabIdx) {
            HasPrefab = true;
            break;
          }
        }
        if (HasPrefab) {
          uint64_t ExHash =
              HashCell(CandX, CandZ, Seed ^ STRUCTURE_EXIST_SALT);
          float DensityRoll = (float)(ExHash & MASK16) / DENSITY_SCALE;
          if (DensityRoll < BiomeDef->StructureDensity) {
            uint64_t KdHash =
                HashCell(CandX, CandZ, Seed ^ STRUCTURE_KIND_SALT);
            if (PickStructurePrefab(BiomeDef, KdHash) == TargetPrefabIdx) {
              int SolidBelow = 0;
              int Surface =
                  CachedTerrainTop(CandX, CandZ, Seed, &SolidBelow);
              if (Surface > SEA_LEVEL && SolidBelow >= GROUND_THICKNESS - 1) {
                float Dx = (float)(CandX - StartX);
                float Dz = (float)(CandZ - StartZ);
                float Dist = sqrtf((Dx * Dx) + (Dz * Dz));
                Found = true;
                BestDist = Dist;
                BestX = CandX;
                BestY = Surface + 1;
                BestZ = CandZ;
              }
            }
          }
        }
      }
      if (Found) {
        break;
      }
      continue;
    }

    int Side = 2 * Ring;
    int CellX = CenterCellX - Ring;
    int CellZ = CenterCellZ - Ring;
    const int STEP_X[4] = {1, 0, -1, 0};
    const int STEP_Z[4] = {0, 1, 0, -1};

    for (int Edge = 0; Edge < 4; Edge++) {
      for (int StepIdx = 0; StepIdx < Side; StepIdx++) {
        if (++Evaluations > MAX_EVALUATIONS) {
          goto ExitPrefabSearch;
        }

        int CandX = 0;
        int CandZ = 0;
        CellCandidate(CellX, CellZ, Seed, &CandX, &CandZ);
        int Prelim = GetPreliminarySurface(CandX, CandZ, Seed);
        int BId = SampleBiomeAt(CandX, Prelim, CandZ, Seed, NULL);
        const BiomeType *BiomeDef = GetBiomeDef(BId);
        if (BiomeDef != NULL && BiomeDef->StructureCount > 0 &&
            BiomeDef->StructureTotalWeight > 0) {
          bool HasPrefab = false;
          for (int StructIdx = 0; StructIdx < BiomeDef->StructureCount; StructIdx++) {
            if (BiomeDef->Structures[StructIdx].PrefabIndex == TargetPrefabIdx) {
              HasPrefab = true;
              break;
            }
          }
          if (HasPrefab) {
            uint64_t ExHash =
                HashCell(CandX, CandZ, Seed ^ STRUCTURE_EXIST_SALT);
            float DensityRoll = (float)(ExHash & MASK16) / DENSITY_SCALE;
            if (DensityRoll < BiomeDef->StructureDensity) {
              uint64_t KdHash =
                  HashCell(CandX, CandZ, Seed ^ STRUCTURE_KIND_SALT);
              if (PickStructurePrefab(BiomeDef, KdHash) == TargetPrefabIdx) {
                int SolidBelow = 0;
                int Surface =
                    CachedTerrainTop(CandX, CandZ, Seed, &SolidBelow);
                if (Surface > SEA_LEVEL &&
                    SolidBelow >= GROUND_THICKNESS - 1) {
                  float Dx = (float)(CandX - StartX);
                  float Dz = (float)(CandZ - StartZ);
                  float Dist = sqrtf((Dx * Dx) + (Dz * Dz));
                  if (!Found || Dist < BestDist) {
                    Found = true;
                    BestDist = Dist;
                    BestX = CandX;
                    BestY = Surface + 1;
                    BestZ = CandZ;
                  }
                }
              }
            }
          }
        }

        CellX += STEP_X[Edge];
        CellZ += STEP_Z[Edge];
      }
    }

    if (Found) {
      break;
    }
  }

ExitPrefabSearch:
  if (Found) {
    if (OutX != NULL) {
      *OutX = BestX;
    }
    if (OutY != NULL) {
      *OutY = BestY;
    }
    if (OutZ != NULL) {
      *OutZ = BestZ;
    }
    if (OutDistance != NULL) {
      *OutDistance = BestDist;
    }
    return true;
  }
  return false;
}

// Salts keep the flora draws independent of the tree grid and of each other, so
// a column's existence roll and its kind roll do not correlate.
#define FLORA_EXIST_SALT 0xF10A0DEC0DE00001ULL
#define FLORA_KIND_SALT 0xF10A0DEC0DE00002ULL

// Pick a flora block from the biome's weighted set using a hash draw. Assumes
// FloraCount > 0 and FloraTotalWeight > 0.
static int PickFloraBlock(const BiomeType *Biome, uint64_t Hash) {
  int Count = Biome->FloraCount;
  if (Count > MAX_BIOME_FLORA) {
    Count = MAX_BIOME_FLORA;
  }
  int Roll = (int)(Hash % (uint64_t)Biome->FloraTotalWeight);
  for (int Idx = 0; Idx < Count; Idx++) {
    Roll -= Biome->Flora[Idx].Weight;
    if (Roll < 0) {
      return Biome->Flora[Idx].BlockId;
    }
  }
  return Biome->Flora[0].BlockId;
}

void PlaceChunkFlora(Chunk *ChunkVal, const ColumnEntry *Column) {
  (void)Column;
  uint64_t Seed = GetWorldSeed();
  const uint64_t MASK16 = 0xFFFFULL;
  const float DENSITY_SCALE = 65536.0F;

  // An all-air or all-solid cube holds no surface with air above it, and a
  // cube wholly at or below sea level grows nothing.
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  if (ChunkVal->SolidBlockCount == 0 ||
      ChunkVal->SolidBlockCount == (int)CHUNK_VOLUME ||
      BaseY + CHUNK_SIZE - 1 <= SEA_LEVEL) {
    return;
  }

  for (int Lx = 0; Lx < CHUNK_SIZE; Lx++) {
    for (int Lz = 0; Lz < CHUNK_SIZE; Lz++) {
      // The highest block of the biome's surface block with air above it, both
      // inside this cube; the flora voxel above the top row would belong to
      // the cube above, which this pass cannot write.
      for (int Ly = CHUNK_SIZE - 2; Ly >= 0; Ly--) {
        if (BaseY + Ly <= SEA_LEVEL) {
          break;
        }
        BlockID Ground = ChunkVal->Data[Lx][Ly][Lz];
        if (Ground == 0 || ChunkVal->Data[Lx][Ly + 1][Lz] != 0) {
          continue;
        }
        const BiomeType *Biome =
            GetBiomeDef(GetChunkBiomeAtLocal(ChunkVal, Lx, Ly, Lz));
        if (Ground != (BlockID)Biome->SurfaceBlockId ||
            strcmp(Biome->Family, "underground") == 0) {
          continue;
        }
        if (Biome->FloraCount > 0 && Biome->FloraTotalWeight > 0) {
          int WorldX = (ChunkVal->ChunkX * CHUNK_SIZE) + Lx;
          int WorldZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + Lz;
          uint64_t ExistHash = HashCell(WorldX, WorldZ, Seed ^ FLORA_EXIST_SALT);
          float Roll = (float)(ExistHash & MASK16) / DENSITY_SCALE;
          if (Roll < Biome->FloraDensity) {
            uint64_t KindHash =
                HashCell(WorldX, WorldZ, Seed ^ FLORA_KIND_SALT);
            ChunkVal->Data[Lx][Ly + 1][Lz] =
                (BlockID)PickFloraBlock(Biome, KindHash);
            ChunkVal->SolidBlockCount++;
          }
        }
        break;
      }
    }
  }
}
