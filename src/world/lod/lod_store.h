#ifndef WORLD_LOD_LOD_STORE_H
#define WORLD_LOD_LOD_STORE_H

#include "world/lod/lod_downsample.h"
#include "world/lod/lod_section.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The resident LOD hierarchy: sections of levels 1..LOD_MAX_LEVEL keyed by
// (level, section x, y, z). A level-L section's coordinates are in units of
// 2^L chunks, so chunk (cx, cy, cz) lies in level-L section floor(c / 2^L).
//
// Data enters as 8^3 patches, each filling one octant of a section, and every
// change is carried up level by level until a level is unchanged or out of
// range. A parent cell is rebuilt only from child cells that hold data, so a
// level keeps what it learned from children that were since evicted.
//
// Residency is a clipmap: each level keeps the sections within its own radius
// of the anchor, so coarser levels reach further. Not thread-safe; one thread
// owns the store.

typedef struct {
  // Horizontal radius per level, in sections of that level, around the
  // anchor's section (dx^2 + dz^2 <= R^2). Index 0 is unused.
  int Radius[LOD_MAX_LEVEL + 1];
  // Vertical radius per level in sections of that level; 0 is unbounded.
  int VerticalRadius[LOD_MAX_LEVEL + 1];
} LodStoreConfig;

typedef struct {
  int32_t X;
  int32_t Y;
  int32_t Z;
  // 1..LOD_MAX_LEVEL when used; LOD_SLOT_EMPTY or LOD_SLOT_TOMBSTONE otherwise.
  int32_t Level;
  LodSection *Section;
} LodStoreSlot;

typedef struct {
  LodStoreConfig Config;
  LodStoreSlot *Slots;
  int Capacity;
  int Count;
  int Tombstones;
  bool HasAnchor;
  int AnchorChunkX;
  int AnchorChunkY;
  int AnchorChunkZ;
  uint64_t Evicted;
} LodStore;

typedef enum {
  LOD_WRITE_APPLIED = 0,
  LOD_WRITE_UNCHANGED,
  // Every cell the patch targets already holds data from real chunks and the
  // patch is synthetic.
  LOD_WRITE_SKIPPED_PRECEDENCE,
  LOD_WRITE_OUT_OF_RANGE,
  LOD_WRITE_NO_MEMORY
} LodWriteResult;

typedef struct {
  int Sections;
  int SectionsPerLevel[LOD_MAX_LEVEL + 1];
  int SectionsPerMode[LOD_STORAGE_COUNT];
  // Section structs plus their storage, the slot table, and the two summed.
  size_t SectionBytes;
  size_t TableBytes;
  size_t TotalBytes;
  uint64_t Evicted;
} LodStoreStats;

// Radius 32 sections at every level, vertically unbounded.
void LodStoreDefaultConfig(LodStoreConfig *Config);

// False when the slot table cannot be allocated.
bool LodStoreInit(LodStore *Store, const LodStoreConfig *Config);
void LodStoreFree(LodStore *Store);

// Moves the clipmap centre and evicts every section now out of range. Before
// the first call every position is in range.
void LodStoreSetAnchor(LodStore *Store, int ChunkX, int ChunkY, int ChunkZ);

bool LodStoreInRange(const LodStore *Store, int Level, int X, int Y, int Z);

const LodSection *LodStoreFind(const LodStore *Store, int Level, int X, int Y,
                               int Z);

// Writes Patch into the octant of the level-Level section that the level
// (Level - 1) cell (ChildX, ChildY, ChildZ) occupies, then propagates upward.
// Level 1 children are chunks. Synthetic data never replaces a 4^3 cell that
// holds real data; real data replaces anything.
LodWriteResult LodStoreWritePatch(LodStore *Store, int Level, int ChildX,
                                  int ChildY, int ChildZ,
                                  const LodPatch8 *Patch, LodOrigin Origin);

static inline LodWriteResult LodStoreWriteChunk(LodStore *Store, int ChunkX,
                                                int ChunkY, int ChunkZ,
                                                const LodPatch8 *Patch,
                                                LodOrigin Origin) {
  return LodStoreWritePatch(Store, 1, ChunkX, ChunkY, ChunkZ, Patch, Origin);
}

void LodStoreGetStats(const LodStore *Store, LodStoreStats *Out);

// Content hash of every section of one level, in coordinate order, so equal
// content gives an equal hash whatever order it was written in. Zero when the
// level is empty or the sort buffer cannot be allocated.
uint64_t LodStoreLevelHash(const LodStore *Store, int Level);

// floor(Value / 2^Shift) for negative values too.
static inline int LodFloorShift(int Value, int Shift) {
  int Divisor = 1 << Shift;
  int Quotient = Value / Divisor;
  return (Value % Divisor < 0) ? Quotient - 1 : Quotient;
}

#endif
