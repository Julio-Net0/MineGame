#include "world/feature.h"
#include "world/chunk.h"
#include "world/prefab.h"
#include "persistence/world_save.h"
#include <stdbool.h>
#include <stdint.h>

enum {
  // World space is tiled into GRID_CELL_SIZE^2 columns; each cell yields at most
  // one tree candidate, which both spaces trees apart and bounds how many cells
  // a chunk's scan has to visit.
  GRID_CELL_SIZE = 6,
  // A cell carries a tree when the low 16 hash bits fall under this threshold.
  // 26214 / 65536 ~= 0.4, so roughly two cells in five are wooded.
  TREE_DENSITY_THRESHOLD = 26214
};

#define TREE_PREFAB_NAME "oak_small"

// SplitMix64: one multiply-xor-shift round per stage, enough to turn adjacent
// seeds and cell indices into uncorrelated bits. Same construction the biome
// sampler uses, kept local so the feature layer pulls in nothing extra.
static uint64_t SplitMix64(uint64_t Value) {
  const uint64_t GOLDEN_GAMMA = 0x9E3779B97F4A7C15ULL;
  const uint64_t MIX_A = 0xBF58476D1CE4E5B9ULL;
  const uint64_t MIX_B = 0x94D049BB133111EBULL;
  const unsigned int SHIFT_A = 30U;
  const unsigned int SHIFT_B = 27U;
  const unsigned int SHIFT_C = 31U;

  Value += GOLDEN_GAMMA;
  Value = (Value ^ (Value >> SHIFT_A)) * MIX_A;
  Value = (Value ^ (Value >> SHIFT_B)) * MIX_B;
  return Value ^ (Value >> SHIFT_C);
}

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

// One candidate per grid cell. Returns false when the cell holds no tree;
// otherwise writes the candidate's world column into OutX/OutZ.
static bool CellCandidate(int CellX, int CellZ, uint64_t Seed, int *OutX,
                          int *OutZ) {
  const unsigned int SHIFT_JITTER_X = 16U;
  const unsigned int SHIFT_JITTER_Z = 32U;
  const uint64_t MASK16 = 0xFFFFULL;

  uint64_t Hash = HashCell(CellX, CellZ, Seed);
  if ((Hash & MASK16) >= (uint64_t)TREE_DENSITY_THRESHOLD) {
    return false;
  }

  int JitterX = (int)((Hash >> SHIFT_JITTER_X) % (uint64_t)GRID_CELL_SIZE);
  int JitterZ = (int)((Hash >> SHIFT_JITTER_Z) % (uint64_t)GRID_CELL_SIZE);
  *OutX = (CellX * GRID_CELL_SIZE) + JitterX;
  *OutZ = (CellZ * GRID_CELL_SIZE) + JitterZ;
  return true;
}

void PlaceChunkFeatures(Chunk *ChunkVal) {
  const Prefab *Tree = GetPrefab(TREE_PREFAB_NAME);
  if (Tree == NULL) {
    return;
  }

  uint64_t Seed = GetWorldSeed();

  // A tree rooted up to its own footprint outside the chunk can still stamp
  // cells into it, so the scan widens the chunk's column range by that reach on
  // every side before mapping to grid cells.
  int Margin = Tree->SizeX > Tree->SizeZ ? Tree->SizeX : Tree->SizeZ;

  int MinX = (ChunkVal->ChunkX * CHUNK_SIZE) - Margin;
  int MaxX = (ChunkVal->ChunkX * CHUNK_SIZE) + CHUNK_SIZE - 1 + Margin;
  int MinZ = (ChunkVal->ChunkZ * CHUNK_SIZE) - Margin;
  int MaxZ = (ChunkVal->ChunkZ * CHUNK_SIZE) + CHUNK_SIZE - 1 + Margin;

  int CellMinX = FloorDiv(MinX, GRID_CELL_SIZE);
  int CellMaxX = FloorDiv(MaxX, GRID_CELL_SIZE);
  int CellMinZ = FloorDiv(MinZ, GRID_CELL_SIZE);
  int CellMaxZ = FloorDiv(MaxZ, GRID_CELL_SIZE);

  for (int CellX = CellMinX; CellX <= CellMaxX; CellX++) {
    for (int CellZ = CellMinZ; CellZ <= CellMaxZ; CellZ++) {
      int WorldX = 0;
      int WorldZ = 0;
      if (!CellCandidate(CellX, CellZ, Seed, &WorldX, &WorldZ)) {
        continue;
      }

      // Skip submerged columns using the same threshold generation floods with;
      // the clip test in the stamp then drops any cell outside this chunk.
      int Surface = GetTerrainHeightAt(WorldX, WorldZ, Seed);
      if (Surface <= SEA_LEVEL) {
        continue;
      }

      StampPrefabIntoChunk(ChunkVal, Tree, WorldX, Surface + 1, WorldZ);
    }
  }
}
