#include "world/column_bounds.h"
#include "core/hash.h"
#include "world/chunk.h"
#include "world/feature.h"
#include "world/worldgen_column_cache.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
  REACH_KEY_SHIFT = 16U,
  RAISED_TOPS_INITIAL_CAPACITY = 256,
  // Grown at half full, which keeps probe runs short.
  RAISED_TOPS_LOAD_DIVISOR = 2
};

static int FloorDivInt(int Value, int Divisor) {
  int Quotient = Value / Divisor;
  if ((Value % Divisor) != 0 && Value < 0) {
    Quotient--;
  }
  return Quotient;
}

static int WrapIndex(int Value) {
  int Rem = Value % COLUMN_BOUNDS_SIDE;
  return (Rem < 0) ? Rem + COLUMN_BOUNDS_SIDE : Rem;
}

void ColumnBoundsInit(ColumnBoundsTable *Table) {
  memset(Table, 0, sizeof(*Table));
}

static ColumnBoundsEntry *SlotFor(ColumnBoundsTable *Table, int ColumnX,
                                  int ColumnZ) {
  size_t Index = ((size_t)WrapIndex(ColumnX) * COLUMN_BOUNDS_SIDE) +
                 (size_t)WrapIndex(ColumnZ);
  return &Table->Entries[Index];
}

// Returns the column's entry, evaluating it when the slot holds another
// column or nothing. Reports whether it had to evaluate.
static ColumnBoundsEntry *Resolve(ColumnBoundsTable *Table, int ColumnX,
                                  int ColumnZ, bool *OutComputed) {
  ColumnBoundsEntry *Entry = SlotFor(Table, ColumnX, ColumnZ);
  if (Entry->Valid && Entry->ColumnX == ColumnX && Entry->ColumnZ == ColumnZ) {
    *OutComputed = false;
    return Entry;
  }
  float High = 0.0F;
  float Low = 0.0F;
  ColumnDensityBounds(ColumnX, ColumnZ, Table->Seed, &High, &Low);
  *Entry = (ColumnBoundsEntry){.ColumnX = ColumnX,
                               .ColumnZ = ColumnZ,
                               .HighBlock = (int16_t)floorf(High),
                               .LowBlock = (int16_t)floorf(Low),
                               .LayerKey = 0U,
                               .Valid = true};
  Table->Computed++;
  *OutComputed = true;
  return Entry;
}

int ColumnBoundsEnsure(ColumnBoundsTable *Table, int CenterX, int CenterZ,
                       int Radius, uint64_t Seed) {
  if (Table->Seed != Seed) {
    size_t Count = sizeof(Table->Entries) / sizeof(Table->Entries[0]);
    for (size_t Idx = 0; Idx < Count; Idx++) {
      Table->Entries[Idx].Valid = false;
    }
    Table->Seed = Seed;
  }

  // A structure rooted up to Footprint blocks outside a column can stamp into
  // it, so its anchors lie in the columns that far around.
  PrefabReach Reach = MaxPrefabReach();
  Table->ReachColumns = (Reach.Footprint + CHUNK_SIZE - 1) / CHUNK_SIZE;
  Table->ReachAbove = Reach.Above;
  // Never 0, which marks an answer as absent.
  Table->LayerKey = (((uint32_t)Table->ReachColumns << REACH_KEY_SHIFT) |
                     (uint32_t)Table->ReachAbove) + 1U;

  int Span = Radius + Table->ReachColumns;
  int SpanSq = Span * Span;
  int Evaluated = 0;
  // Row by row, so neighbouring columns meet their shared lattice corners
  // while the shape memo still holds them.
  for (int Dx = -Span; Dx <= Span; Dx++) {
    for (int Dz = -Span; Dz <= Span; Dz++) {
      if ((Dx * Dx) + (Dz * Dz) > SpanSq) {
        continue;
      }
      bool Computed = false;
      (void)Resolve(Table, CenterX + Dx, CenterZ + Dz, &Computed);
      Evaluated += Computed ? 1 : 0;
    }
  }
  return Evaluated;
}

// Mirrors the structure pass's skip test (CubeOutOfReach): a structure anchors
// on a terrain top, which never lies above its column's BoundHigh, and is
// stamped up to ReachAbove blocks over the cell above it. Terrain itself is
// classified air only above max(BoundHigh, sea level). Flora sits one block
// over the surface, inside the structure term.
static void ResolveLayers(ColumnBoundsTable *Table, ColumnBoundsEntry *Own) {
  int Top = (Own->HighBlock > SEA_LEVEL) ? Own->HighBlock : SEA_LEVEL;
  int LowBlock = Own->LowBlock;
  int ColumnX = Own->ColumnX;
  int ColumnZ = Own->ColumnZ;

  int Reach = Table->ReachColumns;
  for (int Dx = -Reach; Dx <= Reach; Dx++) {
    for (int Dz = -Reach; Dz <= Reach; Dz++) {
      bool Computed = false;
      // May refill a slot, though never Own's: the table is wider than any
      // neighbourhood the reach can span.
      const ColumnBoundsEntry *Near =
          Resolve(Table, ColumnX + Dx, ColumnZ + Dz, &Computed);
      int StructureTop = Near->HighBlock + 1 + Table->ReachAbove;
      Top = (StructureTop > Top) ? StructureTop : Top;
    }
  }

  // Own may have moved under a refill only if the reach exceeds the table; it
  // is looked up again rather than trusted.
  bool Computed = false;
  ColumnBoundsEntry *Entry = Resolve(Table, ColumnX, ColumnZ, &Computed);
  Entry->TopLayer = (int16_t)FloorDivInt(Top, CHUNK_SIZE);
  Entry->BottomLayer = (int16_t)FloorDivInt(LowBlock, CHUNK_SIZE);
  Entry->LayerKey = Table->LayerKey;
}

static const ColumnBoundsEntry *ResolveWithLayers(ColumnBoundsTable *Table,
                                                  int ColumnX, int ColumnZ) {
  bool Computed = false;
  ColumnBoundsEntry *Entry = Resolve(Table, ColumnX, ColumnZ, &Computed);
  if (Entry->LayerKey != Table->LayerKey || Table->LayerKey == 0U) {
    ResolveLayers(Table, Entry);
    Entry = Resolve(Table, ColumnX, ColumnZ, &Computed);
  }
  return Entry;
}

int ColumnBoundsTopLayer(ColumnBoundsTable *Table, int ColumnX, int ColumnZ) {
  return ResolveWithLayers(Table, ColumnX, ColumnZ)->TopLayer;
}

int ColumnBoundsBottomLayer(ColumnBoundsTable *Table, int ColumnX,
                            int ColumnZ) {
  return ResolveWithLayers(Table, ColumnX, ColumnZ)->BottomLayer;
}

// ---------------------------------------------------------------------------
// Raised tops
// ---------------------------------------------------------------------------

void RaisedTopsInit(RaisedTopMap *Map) {
  *Map = (RaisedTopMap){.Slots = (RaisedTopEntry *)0,
                        .Capacity = 0,
                        .Count = 0,
                        .Version = 0U};
}

void RaisedTopsFree(RaisedTopMap *Map) {
  free(Map->Slots);
  RaisedTopsInit(Map);
}

static int RaisedHome(int ColumnX, int ColumnZ, int Capacity) {
  const unsigned int HALF_BITS = 32U;
  uint64_t Packed = ((uint64_t)(uint32_t)ColumnX << HALF_BITS) |
                    (uint64_t)(uint32_t)ColumnZ;
  return (int)(SplitMix64(Packed) % (uint64_t)Capacity);
}

static RaisedTopEntry *RaisedFind(RaisedTopEntry *Slots, int Capacity,
                                  int ColumnX, int ColumnZ) {
  int Index = RaisedHome(ColumnX, ColumnZ, Capacity);
  while (Slots[Index].Used &&
         (Slots[Index].ColumnX != ColumnX || Slots[Index].ColumnZ != ColumnZ)) {
    Index = (Index + 1) % Capacity;
  }
  return &Slots[Index];
}

static bool RaisedGrow(RaisedTopMap *Map) {
  int Capacity = (Map->Capacity == 0) ? RAISED_TOPS_INITIAL_CAPACITY
                                      : Map->Capacity * 2;
  RaisedTopEntry *Slots =
      (RaisedTopEntry *)calloc((size_t)Capacity, sizeof(RaisedTopEntry));
  if (Slots == (RaisedTopEntry *)0) {
    return false;
  }
  for (int Idx = 0; Idx < Map->Capacity; Idx++) {
    const RaisedTopEntry *Old = &Map->Slots[Idx];
    if (Old->Used) {
      *RaisedFind(Slots, Capacity, Old->ColumnX, Old->ColumnZ) = *Old;
    }
  }
  free(Map->Slots);
  Map->Slots = Slots;
  Map->Capacity = Capacity;
  return true;
}

bool RaisedTopsRaise(RaisedTopMap *Map, int ColumnX, int ColumnZ, int Layer) {
  if ((Map->Count + 1) * RAISED_TOPS_LOAD_DIVISOR > Map->Capacity &&
      !RaisedGrow(Map)) {
    return false;
  }
  RaisedTopEntry *Entry = RaisedFind(Map->Slots, Map->Capacity, ColumnX, ColumnZ);
  if (Entry->Used && Entry->TopLayer >= Layer) {
    return false;
  }
  if (!Entry->Used) {
    Map->Count++;
  }
  *Entry = (RaisedTopEntry){.ColumnX = ColumnX,
                            .ColumnZ = ColumnZ,
                            .TopLayer = Layer,
                            .Used = true};
  Map->Version++;
  return true;
}

bool RaisedTopsGet(const RaisedTopMap *Map, int ColumnX, int ColumnZ,
                   int *OutLayer) {
  if (Map->Count == 0) {
    return false;
  }
  const RaisedTopEntry *Entry =
      RaisedFind(Map->Slots, Map->Capacity, ColumnX, ColumnZ);
  if (!Entry->Used) {
    return false;
  }
  *OutLayer = Entry->TopLayer;
  return true;
}
