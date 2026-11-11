#ifndef WORLD_WORLDGEN_COLUMN_CACHE_H
#define WORLD_WORLDGEN_COLUMN_CACHE_H

#include "world/chunk.h"
#include "world/chunk_worker.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Per-thread cache of the 2D data a chunk column needs: the preliminary surface
// of every block column and the climate of every biome cell column. Every cube
// of a column shares it, so a column stacked dozens of cubes high samples its
// 2D noise once instead of once per cube.
//
// Each generation thread owns one cache (workers by index, the main thread its
// own slot), so lookups take no lock. Entries hold exactly what the pure
// samplers return; the cache only avoids recomputing them.

enum {
  COLUMN_CACHE_ENTRIES = 64,
  // Slots 0..CHUNK_WORKER_MAX_THREADS-1 belong to the workers; the last one to
  // the main thread (preview statement, self-tests).
  COLUMN_CACHE_MAIN_SLOT = CHUNK_WORKER_MAX_THREADS,
  COLUMN_CACHE_SLOTS = CHUNK_WORKER_MAX_THREADS + 1
};

typedef struct ColumnEntry {
  int ColumnX;
  int ColumnZ;
  uint64_t Seed;
  bool Valid;
  int16_t Surface[CHUNK_SIZE][CHUNK_SIZE];
  float Temperature[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK];
  float Humidity[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK];
  int16_t MinSurface;
  int16_t MaxSurface;
} ColumnEntry;

typedef struct {
  ColumnEntry Entries[COLUMN_CACHE_ENTRIES];
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
