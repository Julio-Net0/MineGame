#include "world/feature.h"
#include "core/hash.h"
#include "world/chunk.h"
#include "world/surface.h"
#include "world/terrain_density.h"
#include "world/worldgen_column_cache.h"
#include "world/biome.h"
#include "world/prefab.h"
#include "persistence/world_save.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

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
// reach this cube vertically.
static bool CubeOutOfReach(const Chunk *ChunkVal, ColumnCache *Cache,
                           int MinX, int MaxX, int MinZ, int MaxZ,
                           const PrefabReach *Reach, uint64_t Seed) {
  int LowestSurface = 0;
  int HighestSurface = 0;
  bool First = true;
  for (int ColumnX = FloorDiv(MinX, CHUNK_SIZE);
       ColumnX <= FloorDiv(MaxX, CHUNK_SIZE); ColumnX++) {
    for (int ColumnZ = FloorDiv(MinZ, CHUNK_SIZE);
         ColumnZ <= FloorDiv(MaxZ, CHUNK_SIZE); ColumnZ++) {
      const ColumnEntry *Entry =
          ColumnCacheLookup(Cache, ColumnX, ColumnZ, Seed);
      LowestSurface = (First || Entry->MinSurface < LowestSurface)
                          ? Entry->MinSurface
                          : LowestSurface;
      HighestSurface = (First || Entry->MaxSurface > HighestSurface)
                           ? Entry->MaxSurface
                           : HighestSurface;
      First = false;
    }
  }

  // A structure is stamped from S + 1 - Below to S + 1 + Above.
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  int TopY = BaseY + CHUNK_SIZE - 1;
  int ReachLow = LowestSurface + 1 - Reach->Below;
  int ReachHigh = HighestSurface + 1 + Reach->Above;
  return TopY < ReachLow || BaseY > ReachHigh;
}

// The preliminary surface is a 2D estimate: on rugged terrain it can sit in air
// or inside an overhang. A structure is placed only where the density field
// agrees that the anchor block is ground with open space above it.
static bool HasGroundUnderAir(int WorldX, int Surface, int WorldZ,
                              uint64_t Seed) {
  return SampleTerrainDensity(WorldX, Surface, WorldZ, Seed) > 0.0F &&
         SampleTerrainDensity(WorldX, Surface + 1, WorldZ, Seed) <= 0.0F;
}

typedef struct {
  int WorldX;
  int WorldZ;
  int Surface;
  const Prefab *Structure;
} StructureCandidate;

// Everything that decides a grid cell's structure except whether it reaches a
// given cube and whether the ground probe confirms it. The surface comes from
// Cache when given, otherwise from the pure sampler; both return the same value.
static bool ResolveCandidate(int CellX, int CellZ, uint64_t Seed,
                            ColumnCache *Cache, StructureCandidate *Out) {
  const uint64_t MASK16 = 0xFFFFULL;
  const float DENSITY_SCALE = 65536.0F;

  CellCandidate(CellX, CellZ, Seed, &Out->WorldX, &Out->WorldZ);
  int WorldX = Out->WorldX;
  int WorldZ = Out->WorldZ;

  // Skip submerged columns using the same threshold generation floods with;
  // the clip test in the stamp then drops any cell outside this chunk.
  Out->Surface = (Cache != NULL) ? CachedSurface(Cache, WorldX, WorldZ, Seed)
                                 : GetPreliminarySurface(WorldX, WorldZ, Seed);
  if (Out->Surface <= SEA_LEVEL) {
    return false;
  }

  // Resolve the biome at this column's surface (Depth ~= 0) exactly as the pure
  // sampler does, so every chunk re-deriving this overhanging structure agrees
  // on its prefab regardless of load order.
  int BiomeId = (Cache != NULL)
                    ? CachedBiome(Cache, WorldX, Out->Surface, WorldZ, Seed)
                    : SampleBiomeAt(WorldX, Out->Surface, WorldZ, Seed, NULL);
  const BiomeType *Biome = GetBiomeDef(BiomeId);
  if (Biome->StructureCount == 0 || Biome->StructureTotalWeight <= 0) {
    return false;
  }

  uint64_t ExistHash = HashCell(WorldX, WorldZ, Seed ^ STRUCTURE_EXIST_SALT);
  float Roll = (float)(ExistHash & MASK16) / DENSITY_SCALE;
  if (Roll >= Biome->StructureDensity) {
    return false;
  }

  uint64_t KindHash = HashCell(WorldX, WorldZ, Seed ^ STRUCTURE_KIND_SALT);
  Out->Structure = GetPrefabByIndex(PickStructurePrefab(Biome, KindHash));
  return Out->Structure != NULL;
}

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
      // cube; checked before the density probe, which is the costly part.
      const Prefab *Structure = Candidate.Structure;
      int AnchorY = Structure->HasOrigin ? Structure->OriginY : 0;
      int StampLow = Candidate.Surface + 1 - AnchorY;
      int StampHigh = StampLow + Structure->SizeY - 1;
      if (StampHigh < BaseY || StampLow > TopY) {
        continue;
      }
      if (!HasGroundUnderAir(Candidate.WorldX, Candidate.Surface,
                             Candidate.WorldZ, Seed)) {
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

bool FindStructureAnchor(int CellRadius, uint64_t Seed, int *OutX, int *OutY,
                         int *OutZ) {
  for (int CellX = -CellRadius; CellX <= CellRadius; CellX++) {
    for (int CellZ = -CellRadius; CellZ <= CellRadius; CellZ++) {
      StructureCandidate Candidate;
      if (!ResolveCandidate(CellX, CellZ, Seed, NULL, &Candidate) ||
          !HasGroundUnderAir(Candidate.WorldX, Candidate.Surface,
                             Candidate.WorldZ, Seed)) {
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
  uint64_t Seed = GetWorldSeed();
  const uint64_t MASK16 = 0xFFFFULL;
  const float DENSITY_SCALE = 65536.0F;

  // A surface block and the flora voxel above it must both lie in this cube, so
  // a cube the column's surface range cannot reach has nothing to place.
  int BaseY = ChunkVal->ChunkY * CHUNK_SIZE;
  if (Column->MaxSurface < BaseY || Column->MinSurface > BaseY + CHUNK_SIZE - 2) {
    return;
  }

  for (int Lx = 0; Lx < CHUNK_SIZE; Lx++) {
    for (int Lz = 0; Lz < CHUNK_SIZE; Lz++) {
      int WorldX = (ChunkVal->ChunkX * CHUNK_SIZE) + Lx;
      int WorldZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + Lz;

      int Surface = Column->Surface[Lx][Lz];
      if (Surface <= SEA_LEVEL) {
        continue;
      }

      // Both the surface block and the flora voxel above it must live in this
      // chunk: the pass writes only this chunk's data and reads the surface
      // block to check biome support. A column whose surface sits at the very
      // top of the chunk is left to no one, which is a negligible border gap.
      int SurfaceLocalY = Surface - (ChunkVal->ChunkY * CHUNK_SIZE);
      int FloraLocalY = SurfaceLocalY + 1;
      if (SurfaceLocalY < 0 || FloraLocalY >= CHUNK_SIZE) {
        continue;
      }

      unsigned char BiomeId =
          GetChunkBiomeAtLocal(ChunkVal, Lx, SurfaceLocalY, Lz);
      const BiomeType *Biome = GetBiomeDef(BiomeId);
      if (Biome->FloraCount == 0 || Biome->FloraTotalWeight <= 0) {
        continue;
      }

      // Flora grows only on the biome's own surface block (grass), never on
      // exposed stone, sand margins, or an already-occupied voxel.
      if (ChunkVal->Data[Lx][SurfaceLocalY][Lz] !=
          (BlockID)Biome->SurfaceBlockId) {
        continue;
      }
      if (ChunkVal->Data[Lx][FloraLocalY][Lz] != 0) {
        continue;
      }

      uint64_t ExistHash = HashCell(WorldX, WorldZ, Seed ^ FLORA_EXIST_SALT);
      float Roll = (float)(ExistHash & MASK16) / DENSITY_SCALE;
      if (Roll >= Biome->FloraDensity) {
        continue;
      }

      uint64_t KindHash = HashCell(WorldX, WorldZ, Seed ^ FLORA_KIND_SALT);
      int BlockId = PickFloraBlock(Biome, KindHash);

      ChunkVal->Data[Lx][FloraLocalY][Lz] = (BlockID)BlockId;
      ChunkVal->SolidBlockCount++;
    }
  }
}
