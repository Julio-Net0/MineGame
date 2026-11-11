#include "world/worldgen_column_cache.h"
#include "core/hash.h"
#include "core/profiler.h"
#include "world/biome.h"
#include "world/surface.h"
#include "world/terrain_shape.h"
#include <math.h>

static ColumnCache *GetColumnCaches(void) {
  static ColumnCache SCaches[COLUMN_CACHE_SLOTS];
  return SCaches;
}

ColumnCache *GetColumnCache(int Slot) {
  if (Slot < 0 || Slot >= COLUMN_CACHE_SLOTS) {
    Slot = COLUMN_CACHE_MAIN_SLOT;
  }
  return &GetColumnCaches()[Slot];
}

void ColumnEntryFill(ColumnEntry *Entry, int ColumnX, int ColumnZ,
                     uint64_t Seed) {
  const int CELL_CENTER = BIOME_CELL_SIZE / 2;
  int OriginX = ColumnX * CHUNK_SIZE;
  int OriginZ = ColumnZ * CHUNK_SIZE;

  Entry->ColumnX = ColumnX;
  Entry->ColumnZ = ColumnZ;
  Entry->Seed = Seed;

  float BoundHigh = 0.0F;
  float BoundLow = 0.0F;
  for (int CornerX = 0; CornerX < COLUMN_SHAPE_CORNERS; CornerX++) {
    for (int CornerZ = 0; CornerZ < COLUMN_SHAPE_CORNERS; CornerZ++) {
      ColumnShape Shape =
          EvaluateColumnShape(OriginX + (CornerX * SHAPE_LATTICE_STEP),
                              OriginZ + (CornerZ * SHAPE_LATTICE_STEP), Seed);
      Entry->CornerBase[CornerX][CornerZ] = Shape.Base;
      Entry->CornerVariation[CornerX][CornerZ] = Shape.Variation;
      Entry->CornerShell[CornerX][CornerZ] = Shape.ShellDepth;
      float Reach = TERRAIN_NOISE_BOUND * Shape.Variation;
      bool First = (CornerX == 0 && CornerZ == 0);
      BoundHigh = (First || Shape.Base + Reach > BoundHigh) ? Shape.Base + Reach
                                                            : BoundHigh;
      BoundLow = (First || Shape.Base - Reach < BoundLow) ? Shape.Base - Reach
                                                          : BoundLow;
    }
  }
  Entry->BoundHigh = BoundHigh;
  Entry->BoundLow = BoundLow;

  // The same bilinear blend GetPreliminarySurface computes, from the same
  // corners, so the cached surface is bit-identical to the pure one.
  int MinSurface = 0;
  int MaxSurface = 0;
  for (int LocalX = 0; LocalX < CHUNK_SIZE; LocalX++) {
    for (int LocalZ = 0; LocalZ < CHUNK_SIZE; LocalZ++) {
      int CornerX = LocalX / SHAPE_LATTICE_STEP;
      int CornerZ = LocalZ / SHAPE_LATTICE_STEP;
      float Fx = (float)(LocalX % SHAPE_LATTICE_STEP) / (float)SHAPE_LATTICE_STEP;
      float Fz = (float)(LocalZ % SHAPE_LATTICE_STEP) / (float)SHAPE_LATTICE_STEP;
      float Base = ShapeBilinear(Entry->CornerBase[CornerX][CornerZ],
                                 Entry->CornerBase[CornerX + 1][CornerZ],
                                 Entry->CornerBase[CornerX][CornerZ + 1],
                                 Entry->CornerBase[CornerX + 1][CornerZ + 1],
                                 Fx, Fz);
      int Surface = (int)floorf(Base);
      Entry->Surface[LocalX][LocalZ] = (int16_t)Surface;
      if ((LocalX == 0 && LocalZ == 0) || Surface < MinSurface) {
        MinSurface = Surface;
      }
      if ((LocalX == 0 && LocalZ == 0) || Surface > MaxSurface) {
        MaxSurface = Surface;
      }
    }
  }
  Entry->MinSurface = (int16_t)MinSurface;
  Entry->MaxSurface = (int16_t)MaxSurface;

  // Climate is sampled at each biome cell's center column, exactly where the
  // biome map has always sampled it.
  for (int CellX = 0; CellX < BIOME_CELLS_PER_CHUNK; CellX++) {
    for (int CellZ = 0; CellZ < BIOME_CELLS_PER_CHUNK; CellZ++) {
      int LocalX = (CellX * BIOME_CELL_SIZE) + CELL_CENTER;
      int LocalZ = (CellZ * BIOME_CELL_SIZE) + CELL_CENTER;
      int Surface = Entry->Surface[LocalX][LocalZ];
      Entry->Climate[CellX][CellZ] = SampleColumnClimate(
          OriginX + LocalX, OriginZ + LocalZ, Surface, Seed);
      Entry->SurfaceBiome[CellX][CellZ] =
          (uint8_t)ResolveBiome(&Entry->Climate[CellX][CellZ]);

      // The same bilinear blend GetPreliminaryFloor computes, from the same
      // corner levels. A cell centre sits halfway between two lattice columns.
      int CornerX = LocalX / SHAPE_LATTICE_STEP;
      int CornerZ = LocalZ / SHAPE_LATTICE_STEP;
      float Fx = (float)(LocalX % SHAPE_LATTICE_STEP) / (float)SHAPE_LATTICE_STEP;
      float Fz = (float)(LocalZ % SHAPE_LATTICE_STEP) / (float)SHAPE_LATTICE_STEP;
      float Levels[2][2];
      for (int Ix = 0; Ix < 2; Ix++) {
        for (int Iz = 0; Iz < 2; Iz++) {
          Levels[Ix][Iz] =
              ShapeFloorLevel(Entry->CornerBase[CornerX + Ix][CornerZ + Iz],
                              Entry->CornerVariation[CornerX + Ix][CornerZ + Iz]);
        }
      }
      Entry->CellFloor[CellX][CellZ] = (int16_t)floorf(
          ShapeBilinear(Levels[0][0], Levels[1][0], Levels[0][1], Levels[1][1],
                        Fx, Fz));
    }
  }

  Entry->Valid = true;
}

// First entry of the set a column maps to.
static int SetBase(int ColumnX, int ColumnZ) {
  const unsigned int HALF_BITS = 32U;
  uint64_t Packed = ((uint64_t)(uint32_t)ColumnX << HALF_BITS) |
                    (uint64_t)(uint32_t)ColumnZ;
  int Set = (int)(SplitMix64(Packed) % (COLUMN_CACHE_ENTRIES / COLUMN_CACHE_WAYS));
  return Set * COLUMN_CACHE_WAYS;
}

// Set-associative: a column may live in any of its set's ways, and a miss
// replaces the least recently used one. A direct-mapped table made a cube's own
// column and a neighbour the structure pass reads evict each other on every
// cube whenever they hashed to the same slot.
const ColumnEntry *ColumnCacheLookup(ColumnCache *Cache, int ColumnX,
                                     int ColumnZ, uint64_t Seed) {
  int Base = SetBase(ColumnX, ColumnZ);
  Cache->Clock++;

  int Victim = Base;
  for (int Way = 0; Way < COLUMN_CACHE_WAYS; Way++) {
    int Index = Base + Way;
    ColumnEntry *Entry = &Cache->Entries[Index];
    if (Entry->Valid && Entry->ColumnX == ColumnX &&
        Entry->ColumnZ == ColumnZ && Entry->Seed == Seed) {
      Cache->LastUse[Index] = Cache->Clock;
      atomic_fetch_add_explicit(&Cache->Hits, 1, memory_order_relaxed);
      return Entry;
    }
    if (!Entry->Valid ||
        (Cache->Entries[Victim].Valid &&
         Cache->LastUse[Index] < Cache->LastUse[Victim])) {
      Victim = Index;
    }
  }

  atomic_fetch_add_explicit(&Cache->Misses, 1, memory_order_relaxed);
  ColumnEntry *Entry = &Cache->Entries[Victim];
  PROFILE_BEGIN(FillStart);
  ColumnEntryFill(Entry, ColumnX, ColumnZ, Seed);
  PROFILE_END(FillStart, PROFILE_GEN_COLUMN);
  Cache->LastUse[Victim] = Cache->Clock;
  return Entry;
}

void ColumnCacheGetTotals(uint64_t *OutHits, uint64_t *OutMisses) {
  uint64_t Hits = 0;
  uint64_t Misses = 0;
  for (int Slot = 0; Slot < COLUMN_CACHE_SLOTS; Slot++) {
    ColumnCache *Cache = GetColumnCache(Slot);
    Hits += atomic_load_explicit(&Cache->Hits, memory_order_relaxed);
    Misses += atomic_load_explicit(&Cache->Misses, memory_order_relaxed);
  }
  *OutHits = Hits;
  *OutMisses = Misses;
}
