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
// anchored next door can overhang it), evaluate any column not held yet, and
// keep their answer in the column's slot.

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
  // The layer queries' answers, valid while LayerKey equals the table's.
  int16_t TopLayer;
  int16_t BottomLayer;
  uint32_t LayerKey;
  bool Valid;
} ColumnBoundsEntry;

typedef struct {
  ColumnBoundsEntry Entries[COLUMN_BOUNDS_SIDE * COLUMN_BOUNDS_SIDE];
  uint64_t Seed;
  // Columns evaluated since start-up.
  uint64_t Computed;
  // Structure reach the layer queries apply, refreshed by every
  // ColumnBoundsEnsure: neighbour columns on each side and blocks above the
  // anchor. LayerKey changes with them, which drops every cached answer.
  int ReachColumns;
  int ReachAbove;
  uint32_t LayerKey;
  // The disc the last ColumnBoundsEnsure covered, so a pass one column away
  // only visits the columns it adds.
  int EnsuredX;
  int EnsuredZ;
  int EnsuredSpan;
  bool EnsuredValid;
  // ColumnBoundsPrefetch's progress over the square around that disc, reset by
  // every ColumnBoundsEnsure.
  int PrefetchCursor;
  bool PrefetchDone;
} ColumnBoundsTable;

void ColumnBoundsInit(ColumnBoundsTable *Table);

// Evaluates every column within Radius plus the structure reach of the centre
// column that the table does not hold yet, and refreshes the reach. Returns
// how many columns were evaluated.
int ColumnBoundsEnsure(ColumnBoundsTable *Table, int CenterX, int CenterZ,
                       int Radius, uint64_t Seed);

// Evaluates up to Budget columns that the next ColumnBoundsEnsure would add if
// the centre moved by one column on each axis (diagonals included), so a
// crossing finds them held. Spread over idle frames: returns how many it
// evaluated, and 0 at no cost once the ring around the last ensured disc is
// complete.
int ColumnBoundsPrefetch(ColumnBoundsTable *Table, int Budget);

// Highest chunk layer that can hold a non-air block of the column: terrain,
// water up to sea level, flora, or a structure anchored within reach.
int ColumnBoundsTopLayer(ColumnBoundsTable *Table, int ColumnX, int ColumnZ);

// ColumnBoundsTopLayer and ColumnBoundsBottomLayer in one table lookup.
void ColumnBoundsLayers(ColumnBoundsTable *Table, int ColumnX, int ColumnZ,
                        int *OutTop, int *OutBottom);

// ColumnBoundsTopLayer without evaluating anything: false when the table does
// not hold the column's answer for its current reach. Never changes the table,
// so it is safe for columns far outside the load disc.
bool ColumnBoundsPeekTopLayer(const ColumnBoundsTable *Table, int ColumnX,
                              int ColumnZ, int *OutTop);

// Lowest chunk layer that can hold terrain air. Every layer below it is solid
// rock apart from caves and buried structure foundations.
int ColumnBoundsBottomLayer(ColumnBoundsTable *Table, int ColumnX, int ColumnZ);

// Highest chunk layer the player has modified, per column: content the terrain
// bounds know nothing about. Grows only, so a load range that includes it never
// has to shrink back. An open-addressing map, empty until the first entry.
typedef struct {
  int ColumnX;
  int ColumnZ;
  int TopLayer;
  bool Used;
} RaisedTopEntry;

typedef struct {
  RaisedTopEntry *Slots;
  int Capacity;
  int Count;
  // Bumped by every raise, so cached load volumes can tell they went stale.
  uint32_t Version;
} RaisedTopMap;

void RaisedTopsInit(RaisedTopMap *Map);
void RaisedTopsFree(RaisedTopMap *Map);
// Records that the column holds modified content at Layer. Returns true when
// that raised the column's recorded top. Allocation failure leaves the map
// unchanged and returns false.
bool RaisedTopsRaise(RaisedTopMap *Map, int ColumnX, int ColumnZ, int Layer);
// The column's recorded top, when it has one.
bool RaisedTopsGet(const RaisedTopMap *Map, int ColumnX, int ColumnZ,
                   int *OutLayer);

#endif
