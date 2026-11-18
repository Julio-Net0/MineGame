#include "world/column_bounds.h"
#include "world/chunk.h"
#include "world/feature.h"
#include "world/worldgen_column_cache.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

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
static const ColumnBoundsEntry *Resolve(ColumnBoundsTable *Table, int ColumnX,
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
int ColumnBoundsTopLayer(ColumnBoundsTable *Table, int ColumnX, int ColumnZ) {
  bool Computed = false;
  const ColumnBoundsEntry *Own = Resolve(Table, ColumnX, ColumnZ, &Computed);
  int Top = (Own->HighBlock > SEA_LEVEL) ? Own->HighBlock : SEA_LEVEL;

  int Reach = Table->ReachColumns;
  for (int Dx = -Reach; Dx <= Reach; Dx++) {
    for (int Dz = -Reach; Dz <= Reach; Dz++) {
      const ColumnBoundsEntry *Near =
          Resolve(Table, ColumnX + Dx, ColumnZ + Dz, &Computed);
      int StructureTop = Near->HighBlock + 1 + Table->ReachAbove;
      Top = (StructureTop > Top) ? StructureTop : Top;
    }
  }
  return FloorDivInt(Top, CHUNK_SIZE);
}

int ColumnBoundsBottomLayer(ColumnBoundsTable *Table, int ColumnX,
                            int ColumnZ) {
  bool Computed = false;
  const ColumnBoundsEntry *Own = Resolve(Table, ColumnX, ColumnZ, &Computed);
  return FloorDivInt(Own->LowBlock, CHUNK_SIZE);
}
