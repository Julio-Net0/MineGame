#ifndef WORLD_COLUMN_BOUNDS_H
#define WORLD_COLUMN_BOUNDS_H

#include "world/world_limits.h"
#include <stdbool.h>
#include <stdint.h>

// Vertical extent of every chunk column around the load centre, from the
// terrain's density bounds alone (no 3D noise, no generated chunk): above
// TopLayer a column holds only air, and below BottomLayer no terrain air can
// open (cave air can). Pure in (column, seed), so a dedicated server computes
// exactly what a client would.
//
// Raw bounds live in a toroidal table one slot per column, wide enough for the
// largest load disc plus a margin, so a column is evaluated once while it stays
// in range. The layer queries combine a column with its neighbours (a structure
// anchored next door can overhang it) and evaluate any column not held yet.

enum {
  // Columns kept beyond the load disc on every side: the structure reach the
  // layer queries read. A wider reach still works, it only refills slots.
  COLUMN_BOUNDS_MARGIN = 4,
  COLUMN_BOUNDS_SIDE = (2 * (MAX_RENDER_DISTANCE + COLUMN_BOUNDS_MARGIN)) + 1
};

typedef struct {
  int ColumnX;
  int ColumnZ;
  // floor() of the column's BoundHigh and BoundLow, in blocks.
  int16_t HighBlock;
  int16_t LowBlock;
  bool Valid;
} ColumnBoundsEntry;

typedef struct {
  ColumnBoundsEntry Entries[COLUMN_BOUNDS_SIDE * COLUMN_BOUNDS_SIDE];
  uint64_t Seed;
  // Columns evaluated since start-up.
  uint64_t Computed;
  // Structure reach the layer queries apply, refreshed by every
  // ColumnBoundsEnsure: neighbour columns on each side and blocks above the
  // anchor.
  int ReachColumns;
  int ReachAbove;
} ColumnBoundsTable;

void ColumnBoundsInit(ColumnBoundsTable *Table);

// Evaluates every column within Radius plus the structure reach of the centre
// column that the table does not hold yet, and refreshes the reach. Returns
// how many columns were evaluated.
int ColumnBoundsEnsure(ColumnBoundsTable *Table, int CenterX, int CenterZ,
                       int Radius, uint64_t Seed);

// Highest chunk layer that can hold a non-air block of the column: terrain,
// water up to sea level, flora, or a structure anchored within reach.
int ColumnBoundsTopLayer(ColumnBoundsTable *Table, int ColumnX, int ColumnZ);

// Lowest chunk layer that can hold terrain air. Every layer below it is solid
// rock apart from caves and buried structure foundations.
int ColumnBoundsBottomLayer(ColumnBoundsTable *Table, int ColumnX, int ColumnZ);

#endif
