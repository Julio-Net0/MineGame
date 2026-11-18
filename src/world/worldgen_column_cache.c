#include "world/worldgen_column_cache.h"
#include "core/hash.h"
#include "core/profiler.h"
#include "world/biome.h"
#include "world/surface.h"
#include "world/terrain_density.h"
#include "world/terrain_shape.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// A slot's state and its readers share one word, so pinning a READY slot and
// claiming an idle one are single compare-exchanges that cannot interleave.
// Macros, not an enum: READY and the mask do not fit in an int.
#define SLOT_EMPTY 0x00000000U
#define SLOT_FILLING 0x40000000U
#define SLOT_READY 0x80000000U
#define SLOT_STATE_MASK 0xC0000000U

#define ROW_EMPTY 0x00000000U
#define ROW_FILLING 0x40000000U
#define ROW_READY 0x80000000U
#define ROW_STATE_MASK 0xC0000000U

struct SharedColumnSlot {
  atomic_uint Word;
  ColumnEntry Entry;
  CornerRowRing CornerRows;
};

typedef struct {
  ColumnCache *Workers;
  int WorkerCount;
  struct SharedColumnSlot *Table;
  int TableWidth;
} WorkerCaches;

static WorkerCaches *GetWorkerCaches(void) {
  static WorkerCaches Caches = {.Workers = (ColumnCache *)0,
                                .WorkerCount = 0,
                                .Table = (struct SharedColumnSlot *)0,
                                .TableWidth = 0};
  return &Caches;
}

// Static, not allocated: the main thread samples columns before the workers
// exist (spawn search, self-tests) and after they are gone.
static ColumnCache *GetMainColumnCache(void) {
  static ColumnCache MainCache;
  return &MainCache;
}

int ColumnCacheInitWorkers(int Count, int RenderDistance) {
  WorkerCaches *Caches = GetWorkerCaches();
  ColumnCacheFreeWorkers();

  // Sized for the launch render distance. A larger one later only makes columns
  // closer together share slots, which costs fills, never correctness.
  int Width = (2 * (RenderDistance + COLUMN_TABLE_MARGIN)) + 1;
  Caches->Table = (struct SharedColumnSlot *)calloc(
      (size_t)Width * (size_t)Width, sizeof(struct SharedColumnSlot));
  Caches->TableWidth =
      (Caches->Table != (struct SharedColumnSlot *)0) ? Width : 0;

  // Halved on failure: fewer workers with a cache each beats none starting.
  for (int Want = Count; Want > 0; Want /= 2) {
    ColumnCache *Workers =
        (ColumnCache *)calloc((size_t)Want, sizeof(ColumnCache));
    if (Workers != (ColumnCache *)0) {
      for (int Idx = 0; Idx < Want; Idx++) {
        Workers[Idx].UseSharedTable = Caches->TableWidth > 0;
      }
      Caches->Workers = Workers;
      Caches->WorkerCount = Want;
      return Want;
    }
  }
  return 0;
}

void ColumnCacheFreeWorkers(void) {
  WorkerCaches *Caches = GetWorkerCaches();
  free(Caches->Workers);
  Caches->Workers = (ColumnCache *)0;
  Caches->WorkerCount = 0;
  free(Caches->Table);
  Caches->Table = (struct SharedColumnSlot *)0;
  Caches->TableWidth = 0;
}

ColumnCache *GetColumnCache(int Slot) {
  WorkerCaches *Caches = GetWorkerCaches();
  if (Slot >= 0 && Slot < Caches->WorkerCount) {
    return &Caches->Workers[Slot];
  }
  return GetMainColumnCache();
}

// Widens [Low, High] to Base +- TERRAIN_NOISE_BOUND * Variation of one corner.
// Shared by both bound producers so they cannot diverge.
static void AccumulateShapeBounds(const ColumnShape *Shape, bool First,
                                  float *High, float *Low) {
  float Reach = TERRAIN_NOISE_BOUND * Shape->Variation;
  *High = (First || Shape->Base + Reach > *High) ? Shape->Base + Reach : *High;
  *Low = (First || Shape->Base - Reach < *Low) ? Shape->Base - Reach : *Low;
}

void ColumnDensityBounds(int ColumnX, int ColumnZ, uint64_t Seed,
                         float *OutHigh, float *OutLow) {
  int OriginX = ColumnX * CHUNK_SIZE;
  int OriginZ = ColumnZ * CHUNK_SIZE;
  float High = 0.0F;
  float Low = 0.0F;
  for (int CornerX = 0; CornerX < COLUMN_SHAPE_CORNERS; CornerX++) {
    for (int CornerZ = 0; CornerZ < COLUMN_SHAPE_CORNERS; CornerZ++) {
      ColumnShape Shape =
          EvaluateColumnShape(OriginX + (CornerX * SHAPE_LATTICE_STEP),
                              OriginZ + (CornerZ * SHAPE_LATTICE_STEP), Seed);
      AccumulateShapeBounds(&Shape, CornerX == 0 && CornerZ == 0, &High, &Low);
    }
  }
  *OutHigh = High;
  *OutLow = Low;
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
      AccumulateShapeBounds(&Shape, CornerX == 0 && CornerZ == 0, &BoundHigh,
                            &BoundLow);
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

static void FillCounted(ColumnCache *Cache, ColumnEntry *Entry, int ColumnX,
                        int ColumnZ, uint64_t Seed) {
  atomic_fetch_add_explicit(&Cache->Misses, 1, memory_order_relaxed);
  PROFILE_BEGIN(FillStart);
  ColumnEntryFill(Entry, ColumnX, ColumnZ, Seed);
  PROFILE_END(FillStart, PROFILE_GEN_COLUMN);
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

  ColumnEntry *Entry = &Cache->Entries[Victim];
  FillCounted(Cache, Entry, ColumnX, ColumnZ, Seed);
  Cache->LastUse[Victim] = Cache->Clock;
  return Entry;
}

static int Wrap(int Value, int Width) {
  int Rem = Value % Width;
  return (Rem < 0) ? Rem + Width : Rem;
}

static bool EntryHolds(const ColumnEntry *Entry, int ColumnX, int ColumnZ,
                       uint64_t Seed) {
  return Entry->ColumnX == ColumnX && Entry->ColumnZ == ColumnZ &&
         Entry->Seed == Seed;
}

// Returns the slot's entry for the column, pinned, or NULL when the slot is
// being filled or another column in it is still being read.
static const ColumnEntry *SharedAcquire(ColumnCache *Cache,
                                        struct SharedColumnSlot *Slot,
                                        int ColumnX, int ColumnZ,
                                        uint64_t Seed) {
  unsigned int Word = atomic_load_explicit(&Slot->Word, memory_order_relaxed);
  while ((Word & SLOT_STATE_MASK) == SLOT_READY) {
    // Acquire pairs with the release that published READY, so the entry is
    // read only after the fill that wrote it.
    if (atomic_compare_exchange_weak_explicit(&Slot->Word, &Word, Word + 1U,
                                              memory_order_acquire,
                                              memory_order_relaxed)) {
      if (EntryHolds(&Slot->Entry, ColumnX, ColumnZ, Seed)) {
        atomic_fetch_add_explicit(&Cache->Hits, 1, memory_order_relaxed);
        return &Slot->Entry;
      }
      Word = atomic_fetch_sub_explicit(&Slot->Word, 1U, memory_order_release) -
             1U;
      break;
    }
  }

  // Claimed only while idle: empty, or READY with no reader left. Acquire pairs
  // with the last readers' release, so their reads finish before the refill.
  if (Word != SLOT_EMPTY && Word != SLOT_READY) {
    return (const ColumnEntry *)0;
  }
  if (!atomic_compare_exchange_strong_explicit(
          &Slot->Word, &Word, SLOT_FILLING | 1U, memory_order_acquire,
          memory_order_relaxed)) {
    return (const ColumnEntry *)0;
  }
  // FILLING turns every other thread away, so this thread alone writes the
  // entry and the word until the release below publishes it, still pinned.
  // The ring is emptied with it: a plane of the column that held the slot
  // before must never be read back as this column's at the same Y.
  CornerRowRingReset(&Slot->CornerRows);
  FillCounted(Cache, &Slot->Entry, ColumnX, ColumnZ, Seed);
  atomic_store_explicit(&Slot->Word, SLOT_READY | 1U, memory_order_release);
  return &Slot->Entry;
}

const ColumnEntry *ColumnCacheAcquire(ColumnCache *Cache, int ColumnX,
                                      int ColumnZ, uint64_t Seed,
                                      ColumnHandle *Handle) {
  Handle->Slot = (struct SharedColumnSlot *)0;
  WorkerCaches *Caches = GetWorkerCaches();
  if (Cache->UseSharedTable && Caches->TableWidth > 0) {
    int Width = Caches->TableWidth;
    struct SharedColumnSlot *Slot =
        &Caches->Table[((size_t)Wrap(ColumnX, Width) * (size_t)Width) +
                       (size_t)Wrap(ColumnZ, Width)];
    const ColumnEntry *Entry =
        SharedAcquire(Cache, Slot, ColumnX, ColumnZ, Seed);
    if (Entry != (const ColumnEntry *)0) {
      Handle->Slot = Slot;
      return Entry;
    }
    atomic_fetch_add_explicit(&Cache->Fallbacks, 1, memory_order_relaxed);
  }
  return ColumnCacheLookup(Cache, ColumnX, ColumnZ, Seed);
}

void ColumnCacheRelease(ColumnHandle *Handle) {
  if (Handle->Slot != (struct SharedColumnSlot *)0) {
    atomic_fetch_sub_explicit(&Handle->Slot->Word, 1U, memory_order_release);
    Handle->Slot = (struct SharedColumnSlot *)0;
  }
}

CornerRowRing *ColumnHandleCornerRows(const ColumnHandle *Handle) {
  return (Handle->Slot != (struct SharedColumnSlot *)0)
             ? &Handle->Slot->CornerRows
             : (CornerRowRing *)0;
}

void CornerRowRingReset(CornerRowRing *Ring) {
  for (int Idx = 0; Idx < CORNER_ROW_RING; Idx++) {
    atomic_store_explicit(&Ring->Rows[Idx].State, ROW_EMPTY,
                          memory_order_relaxed);
  }
}

static CornerRow *RingRow(CornerRowRing *Ring, int PlaneY) {
  return &Ring->Rows[Wrap(PlaneY / DENSITY_CELL_Y, CORNER_ROW_RING)];
}

bool CornerRowRingLoad(CornerRowRing *Ring, int PlaneY,
                       float Out[COLUMN_SHAPE_CORNERS][COLUMN_SHAPE_CORNERS]) {
  CornerRow *Row = RingRow(Ring, PlaneY);
  unsigned int Word = atomic_load_explicit(&Row->State, memory_order_relaxed);
  while ((Word & ROW_STATE_MASK) == ROW_READY) {
    // Acquire pairs with the release that published READY: the plane is read
    // only after the store that wrote it, and stays put while pinned.
    if (atomic_compare_exchange_weak_explicit(&Row->State, &Word, Word + 1U,
                                              memory_order_acquire,
                                              memory_order_relaxed)) {
      bool Holds = Row->PlaneY == PlaneY;
      if (Holds) {
        memcpy(Out, Row->Values, sizeof(Row->Values));
      }
      atomic_fetch_sub_explicit(&Row->State, 1U, memory_order_release);
      return Holds;
    }
  }
  return false;
}

void CornerRowRingStore(
    CornerRowRing *Ring, int PlaneY,
    const float Values[COLUMN_SHAPE_CORNERS][COLUMN_SHAPE_CORNERS]) {
  CornerRow *Row = RingRow(Ring, PlaneY);
  // Claimed only while idle: empty, or READY with no reader left. Acquire pairs
  // with the last readers' release, so their copies finish before the rewrite.
  unsigned int Word = atomic_load_explicit(&Row->State, memory_order_relaxed);
  if (Word != ROW_EMPTY && Word != ROW_READY) {
    return;
  }
  if (!atomic_compare_exchange_strong_explicit(&Row->State, &Word, ROW_FILLING,
                                               memory_order_acquire,
                                               memory_order_relaxed)) {
    return;
  }
  Row->PlaneY = PlaneY;
  memcpy(Row->Values, Values, sizeof(Row->Values));
  atomic_store_explicit(&Row->State, ROW_READY, memory_order_release);
}

void ColumnCacheGetTotals(uint64_t *OutHits, uint64_t *OutMisses) {
  WorkerCaches *Caches = GetWorkerCaches();
  // The main cache sits after the workers' in this walk.
  uint64_t Hits = 0;
  uint64_t Misses = 0;
  for (int Slot = 0; Slot <= Caches->WorkerCount; Slot++) {
    ColumnCache *Cache = (Slot < Caches->WorkerCount) ? &Caches->Workers[Slot]
                                                      : GetMainColumnCache();
    Hits += atomic_load_explicit(&Cache->Hits, memory_order_relaxed);
    Misses += atomic_load_explicit(&Cache->Misses, memory_order_relaxed);
  }
  *OutHits = Hits;
  *OutMisses = Misses;
}

uint64_t ColumnCacheGetFallbacks(void) {
  WorkerCaches *Caches = GetWorkerCaches();
  uint64_t Fallbacks = 0;
  for (int Slot = 0; Slot < Caches->WorkerCount; Slot++) {
    Fallbacks += atomic_load_explicit(&Caches->Workers[Slot].Fallbacks,
                                      memory_order_relaxed);
  }
  return Fallbacks;
}
