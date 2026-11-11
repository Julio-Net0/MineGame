#include "world/worldgen_column_cache.h"
#include "core/hash.h"
#include "core/profiler.h"
#include "world/biome.h"
#include "world/surface.h"

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

  int MinSurface = 0;
  int MaxSurface = 0;
  for (int LocalX = 0; LocalX < CHUNK_SIZE; LocalX++) {
    for (int LocalZ = 0; LocalZ < CHUNK_SIZE; LocalZ++) {
      int Surface =
          GetPreliminarySurface(OriginX + LocalX, OriginZ + LocalZ, Seed);
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
      BiomeClimate Climate = SampleBiomeClimate(
          OriginX + LocalX, Surface, OriginZ + LocalZ, Surface, Seed);
      Entry->Temperature[CellX][CellZ] = Climate.Temperature;
      Entry->Humidity[CellX][CellZ] = Climate.Humidity;
    }
  }

  Entry->Valid = true;
}

static int EntryIndex(int ColumnX, int ColumnZ) {
  const unsigned int HALF_BITS = 32U;
  uint64_t Packed = ((uint64_t)(uint32_t)ColumnX << HALF_BITS) |
                    (uint64_t)(uint32_t)ColumnZ;
  return (int)(SplitMix64(Packed) % COLUMN_CACHE_ENTRIES);
}

const ColumnEntry *ColumnCacheLookup(ColumnCache *Cache, int ColumnX,
                                     int ColumnZ, uint64_t Seed) {
  ColumnEntry *Entry = &Cache->Entries[EntryIndex(ColumnX, ColumnZ)];
  if (Entry->Valid && Entry->ColumnX == ColumnX && Entry->ColumnZ == ColumnZ &&
      Entry->Seed == Seed) {
    atomic_fetch_add_explicit(&Cache->Hits, 1, memory_order_relaxed);
    return Entry;
  }

  atomic_fetch_add_explicit(&Cache->Misses, 1, memory_order_relaxed);
  PROFILE_BEGIN(FillStart);
  ColumnEntryFill(Entry, ColumnX, ColumnZ, Seed);
  PROFILE_END(FillStart, PROFILE_GEN_COLUMN);
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
