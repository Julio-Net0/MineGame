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
//
// Workers share one more level in front of their caches: a table with one slot
// per column of a toroidal grid covering the loaded area, so a column is filled
// once for every worker instead of once per worker that meets it (its owner,
// and each neighbour's owner the structure pass reads it for). A slot is pinned
// by a use count while read and only refilled when no one holds it.

enum {
  COLUMN_CACHE_ENTRIES = 256,
  COLUMN_CACHE_WAYS = 4,
  // Slots 0..ThreadCount-1 belong to the workers; any other slot, this one
  // included, to the main thread (preview statement, self-tests).
  COLUMN_CACHE_MAIN_SLOT = CHUNK_WORKER_MAX_THREADS,
  COLUMN_SHAPE_CORNERS = (CHUNK_SIZE / 4) + 1,
  // Columns the shared table covers beyond the render distance on each side:
  // the load ring past it and the structure pass's neighbour reach.
  COLUMN_TABLE_MARGIN = 3,
  // Corner planes a shared column slot keeps, indexed by plane Y modulo the
  // count: 8 planes span 64 blocks, the 4 planes of a cube plus its neighbours'.
  CORNER_ROW_RING = 8
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
  // Worker caches go through the shared column table first and keep their own
  // entries only as its fallback; the main thread's cache never does.
  bool UseSharedTable;
  // Written by the owning thread, read by the main thread for reporting.
  atomic_uint_fast64_t Hits;
  atomic_uint_fast64_t Misses;
  atomic_uint_fast64_t Fallbacks;
} ColumnCache;

// Allocates one empty cache per worker, once, before the workers start, and the
// shared column table sized for RenderDistance. A cache is a few hundred
// kilobytes, so the maximum worker count is not paid for on machines that run
// fewer. Returns how many were allocated, at most Count; fewer only when memory
// ran out. Without a shared table the workers run on their own caches alone.
int ColumnCacheInitWorkers(int Count, int RenderDistance);
// Frees the worker caches and the shared table once the workers have stopped.
void ColumnCacheFreeWorkers(void);

ColumnCache *GetColumnCache(int Slot);

// Returns the entry for (ColumnX, ColumnZ, Seed), filling it on a miss. The
// pointer stays valid until the next lookup on the same cache. Never touches
// the shared table.
const ColumnEntry *ColumnCacheLookup(ColumnCache *Cache, int ColumnX,
                                     int ColumnZ, uint64_t Seed);

// A use of a shared table slot, held from ColumnCacheAcquire to
// ColumnCacheRelease. Slot is NULL when the entry came from the private cache.
struct SharedColumnSlot;
typedef struct ColumnHandle {
  struct SharedColumnSlot *Slot;
} ColumnHandle;

// The entry for (ColumnX, ColumnZ, Seed). On a worker cache it comes from the
// shared table, where every column is filled once for all workers, and stays
// valid and unchanged until ColumnCacheRelease. When the column's slot is being
// filled, or still in use by another column (a teleport), the private cache
// serves it instead, and the pointer then follows ColumnCacheLookup's rule.
// Every Acquire must be paired with a Release on the same handle.
const ColumnEntry *ColumnCacheAcquire(ColumnCache *Cache, int ColumnX,
                                      int ColumnZ, uint64_t Seed,
                                      ColumnHandle *Handle);
void ColumnCacheRelease(ColumnHandle *Handle);

// One horizontal plane of a column's density corners, at block Y PlaneY. The
// top two planes a cube evaluates are the bottom two of the cube above it, so
// a plane kept here is evaluated once for both. Values are what the pure corner
// function returns, so a reused plane is bit-identical to a recomputed one.
//
// State is ROW_EMPTY, ROW_FILLING, or ROW_READY plus a reader count, the same
// scheme as the slot word: a reader pins a READY row while it copies, and a
// writer claims only an idle row. Contention never waits; the caller computes
// the plane itself instead.
typedef struct {
  atomic_uint State;
  int PlaneY;
  float Values[COLUMN_SHAPE_CORNERS][COLUMN_SHAPE_CORNERS];
} CornerRow;

typedef struct CornerRowRing {
  CornerRow Rows[CORNER_ROW_RING];
} CornerRowRing;

// The ring of the shared slot Handle pins, or NULL for a private entry, whose
// cubes evaluate every plane themselves.
CornerRowRing *ColumnHandleCornerRows(const ColumnHandle *Handle);
// Only while no other thread can reach the ring.
void CornerRowRingReset(CornerRowRing *Ring);
// Copies the plane at PlaneY into Out; false when the ring does not hold it or
// its row is being written.
bool CornerRowRingLoad(CornerRowRing *Ring, int PlaneY,
                       float Out[COLUMN_SHAPE_CORNERS][COLUMN_SHAPE_CORNERS]);
// Keeps the plane at PlaneY, replacing whatever its row held; skipped when the
// row is being read or written.
void CornerRowRingStore(
    CornerRowRing *Ring, int PlaneY,
    const float Values[COLUMN_SHAPE_CORNERS][COLUMN_SHAPE_CORNERS]);

// Fills Entry from the pure samplers, bypassing any cache. Lookups use it on a
// miss; the self-test uses it to produce the uncached reference.
void ColumnEntryFill(ColumnEntry *Entry, int ColumnX, int ColumnZ,
                     uint64_t Seed);

// Totals across every slot. A miss is a column fill, in the shared table or a
// private cache.
void ColumnCacheGetTotals(uint64_t *OutHits, uint64_t *OutMisses);
// Worker acquisitions the shared table could not serve.
uint64_t ColumnCacheGetFallbacks(void);

#endif
