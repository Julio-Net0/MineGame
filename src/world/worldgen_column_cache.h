#ifndef WORLD_WORLDGEN_COLUMN_CACHE_H
#define WORLD_WORLDGEN_COLUMN_CACHE_H

#include "world/chunk.h"
#include "world/chunk_worker.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Per-thread cache of the 2D data a chunk column needs: the preliminary surface
// of every block column, and the climate and surface biome of every biome cell
// column. Every cube
// of a column shares it, so a column stacked dozens of cubes high samples its
// 2D noise once instead of once per cube.
//
// Each generation thread owns one cache (workers by index, the main thread its
// own slot), so lookups take no lock. Entries hold exactly what the pure
// samplers return; the cache only avoids recomputing them.

enum {
  COLUMN_CACHE_ENTRIES = 256,
  COLUMN_CACHE_WAYS = 4,
  // Slots 0..CHUNK_WORKER_MAX_THREADS-1 belong to the workers; the last one to
  // the main thread (preview statement, self-tests).
  COLUMN_CACHE_MAIN_SLOT = CHUNK_WORKER_MAX_THREADS,
  COLUMN_CACHE_SLOTS = CHUNK_WORKER_MAX_THREADS + 1,
  COLUMN_SHAPE_CORNERS = (CHUNK_SIZE / 4) + 1
};

typedef struct ColumnEntry {
  int ColumnX;
  int ColumnZ;
  uint64_t Seed;
  bool Valid;
  // Terrain shape at the 5x5 lattice columns covering this column; the last row
  // and column belong to the neighbouring column too, so values are shared.
  float CornerBase[COLUMN_SHAPE_CORNERS][COLUMN_SHAPE_CORNERS];
  float CornerVariation[COLUMN_SHAPE_CORNERS][COLUMN_SHAPE_CORNERS];
  float CornerShell[COLUMN_SHAPE_CORNERS][COLUMN_SHAPE_CORNERS];
  // Bounds of Base +- TERRAIN_NOISE_BOUND * Variation over those corners: no
  // overworld block of the column can be solid above BoundHigh or non-solid
  // below BoundLow.
  float BoundHigh;
  float BoundLow;
  int16_t Surface[CHUNK_SIZE][CHUNK_SIZE];
  // Climate of each biome cell column (Depth 0), and the biome it resolves to
  // anywhere in the surface depth band.
  BiomeClimate Climate[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK];
  uint8_t SurfaceBiome[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK];
  // Preliminary floor at each biome cell's centre column: biome depth is
  // measured from it.
  int16_t CellFloor[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK];
  int16_t MinSurface;
  int16_t MaxSurface;
} ColumnEntry;

typedef struct ColumnCache {
  ColumnEntry Entries[COLUMN_CACHE_ENTRIES];
  // Owner-thread only: recency per entry for replacement within a set.
  uint32_t LastUse[COLUMN_CACHE_ENTRIES];
  uint32_t Clock;
  // Written by the owning thread, read by the main thread for reporting.
  atomic_uint_fast64_t Hits;
  atomic_uint_fast64_t Misses;
} ColumnCache;

ColumnCache *GetColumnCache(int Slot);

// Returns the entry for (ColumnX, ColumnZ, Seed), filling it on a miss. The
// pointer stays valid until the next lookup on the same cache.
const ColumnEntry *ColumnCacheLookup(ColumnCache *Cache, int ColumnX,
                                     int ColumnZ, uint64_t Seed);

// Fills Entry from the pure samplers, bypassing any cache. Lookups use it on a
// miss; the self-test uses it to produce the uncached reference.
void ColumnEntryFill(ColumnEntry *Entry, int ColumnX, int ColumnZ,
                     uint64_t Seed);

// Totals across every slot.
void ColumnCacheGetTotals(uint64_t *OutHits, uint64_t *OutMisses);

#endif
