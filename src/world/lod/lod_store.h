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
  // Most sections held DENSE at once while writes arrive (16 KB each); past
  // it the oldest is compacted immediately.
  int HotLimit;
  // Only real data travels up: a parent is written only from the child
  // octants holding real cells, and a whole-section write (synthetic or
  // cached) into a section holding real cells propagates it again. A parent's
  // real cells then always come from its children's final content, whatever
  // order real chunks and whole sections arrived in; everything else comes
  // from the level's own whole sections. For stores filled level by level
  // (the client's); off by default, where synthetic patches propagate too.
  bool RealPropagation;
} LodStoreConfig;

typedef struct {
  int32_t X;
  int32_t Y;
  int32_t Z;
  // 1..LOD_MAX_LEVEL when used; LOD_SLOT_EMPTY or LOD_SLOT_TOMBSTONE otherwise.
  int32_t Level;
  LodSection *Section;
} LodStoreSlot;

typedef struct LodStoreScratch LodStoreScratch;

typedef struct {
  int32_t X;
  int32_t Y;
  int32_t Z;
  int32_t Level;
} LodStoreKey;

// Called for every evicted section still marked Unsaved, just before the store
// frees it. The hook may move the content out with LodSectionAdoptContent
// (and read its provenance); the store frees whatever is left.
typedef void (*LodEvictHook)(void *User, LodStoreKey Key, LodSection *Section);

typedef struct {
  LodStoreConfig Config;
  LodStoreSlot *Slots;
  // Write and palette workspaces, allocated once with the store.
  LodStoreScratch *Scratch;
  LodEncodeScratch *Encode;
  // Storage bytes of every resident section, kept current on each change.
  size_t HeapBytes;
  // Sections changed since their parent was last rebuilt, in change order;
  // each appears once however many writes it took.
  LodStoreKey *Pending;
  int PendingCount;
  int PendingCapacity;
  // Sections held DENSE since their last write, oldest first from HotHead;
  // HotLive counts the ones still hot (entries can go stale on eviction).
  LodStoreKey *Hot;
  int HotHead;
  int HotCount;
  int HotCapacity;
  int HotLive;
  // Dense buffers kept for reuse, so a section turning hot allocates nothing.
  LodVoxel **DensePool;
  int DensePoolCount;
  int Capacity;
  int Count;
  int Tombstones;
  bool HasAnchor;
  int AnchorChunkX;
  int AnchorChunkY;
  int AnchorChunkZ;
  uint64_t Evicted;
  // Every content change and eviction, as a ring that readers poll: slot
  // Seq & (LOD_JOURNAL_SIZE - 1) holds change number Seq. A reader that fell
  // more than LOD_JOURNAL_SIZE changes behind has lost some and must treat
  // every section as changed.
  LodStoreKey *Journal;
  uint64_t JournalSeq;
  LodEvictHook EvictHook;
  void *EvictUser;
} LodStore;

enum { LOD_JOURNAL_SIZE = 8192 };

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
  // Sections holding any real cell, and the others with present cells (all
  // synthetic).
  int SectionsIngested;
  int SectionsSynthetic;
  // Section structs plus their storage, the slot table, the write workspaces,
  // and all of them summed.
  size_t SectionBytes;
  size_t TableBytes;
  size_t ScratchBytes;
  // Spare dense buffers kept for reuse.
  size_t PoolBytes;
  size_t TotalBytes;
  uint64_t Evicted;
  // Sections still DENSE waiting to be compacted.
  int HotSections;
} LodStoreStats;

// Radius 32 sections at every level, vertically unbounded, 512 hot sections.
void LodStoreDefaultConfig(LodStoreConfig *Config);

// False when the slot table cannot be allocated.
bool LodStoreInit(LodStore *Store, const LodStoreConfig *Config);
void LodStoreFree(LodStore *Store);

// Moves the clipmap centre and evicts every section now out of range. Before
// the first call every position is in range.
void LodStoreSetAnchor(LodStore *Store, int ChunkX, int ChunkY, int ChunkZ);

// Replaces the radii, evicting what falls outside them once anchored.
void LodStoreSetConfig(LodStore *Store, const LodStoreConfig *Config);

// Everything the store holds, in O(1): what LodStoreGetStats totals.
size_t LodStoreBytes(const LodStore *Store);
size_t LodStoreScratchBytes(void);

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

// The same write without the propagation: the section joins the pending set
// and LodStoreFlush carries it up. Many writes into one region between flushes
// then rebuild each parent once instead of once per write.
LodWriteResult LodStoreWritePatchDeferred(LodStore *Store, int Level,
                                          int ChildX, int ChildY, int ChildZ,
                                          const LodPatch8 *Patch,
                                          LodOrigin Origin);

// One real chunk's patch for LodStoreWriteChunksDeferred. Sequence orders
// writes to the same chunk: the highest wins.
typedef struct {
  int32_t ChunkX;
  int32_t ChunkY;
  int32_t ChunkZ;
  int32_t Sequence;
  const LodPatch8 *Patch;
} LodChunkWrite;

typedef struct {
  int Applied;
  int OutOfRange;
  int Failed;
} LodBatchResult;

// Deferred level-1 writes of many real chunks at once. Writes into the same
// section share one decode and one encode, which is most of a write's cost.
// Reorders Writes in place. The result equals writing them one at a time in
// Sequence order.
void LodStoreWriteChunksDeferred(LodStore *Store, LodChunkWrite *Writes,
                                 int Count, LodBatchResult *Out);

// Propagates every pending section to the top and compacts every hot section.
// The result does not depend on the order writes were made in. False when
// memory ran out on the way.
bool LodStoreFlush(LodStore *Store);

// Compacts at most MaxEntries hot sections, oldest first, back to their
// smallest storage. Content and hashes are unchanged; only memory shrinks.
bool LodStoreCompactSome(LodStore *Store, int MaxEntries);
bool LodStoreCompactAll(LodStore *Store);

// Frees spare dense buffers beyond Keep, for when no writes are expected soon.
void LodStoreTrimPool(LodStore *Store, int Keep);

// Propagates at most MaxEntries pending sections, oldest first, leaving the
// rest pending. A caller with a time budget calls it in small batches; what
// stays pending keeps collecting changes, so each section is still rebuilt once
// however many writes reach it before its turn.
bool LodStoreFlushSome(LodStore *Store, int MaxEntries);

static inline LodWriteResult LodStoreWriteChunk(LodStore *Store, int ChunkX,
                                                int ChunkY, int ChunkZ,
                                                const LodPatch8 *Patch,
                                                LodOrigin Origin) {
  return LodStoreWritePatch(Store, 1, ChunkX, ChunkY, ChunkZ, Patch, Origin);
}

// Writes one whole synthetic section (see lod_gen.h) from its encoded form,
// taking Encoded's storage when no cell of the section holds real data, so
// the write costs no encoding. Cells holding real data keep their voxels and
// provenance; the others become synthetic, present as PresentCells says. The
// write is not propagated and never leaves the section hot: each level is
// generated on its own, and the ingestion's pending and hot sets stay as
// they were. LOD_WRITE_SKIPPED_PRECEDENCE when every cell holds real data.
LodWriteResult LodStoreWriteSynthetic(LodStore *Store, int Level, int X, int Y,
                                      int Z, LodSection *Encoded,
                                      uint64_t PresentCells);

// Writes one section loaded from the disk cache (lod_cache.h) like
// LodStoreWriteSynthetic, but carrying its provenance: cells the cache holds
// as real stay real. Cells already holding real data in the store keep it,
// since this session's chunks are newer than any cache. The section is marked
// Unsaved only when it no longer equals the cached copy; GeneratedColumn is
// OR-ed into its flag.
LodWriteResult LodStoreWriteCached(LodStore *Store, int Level, int X, int Y,
                                   int Z, LodSection *Encoded,
                                   uint64_t PresentCells, uint64_t IngestedCells,
                                   bool GeneratedColumn);

// Flags the resident level-Level section (X, Y, Z) as part of a column the
// synthetic fill generated whole (LodSection.GeneratedColumn), marking it
// Unsaved when the flag changes. Nothing when it is not resident.
void LodStoreMarkGenerated(LodStore *Store, int Level, int X, int Y, int Z);

// Clears the Unsaved flag of the resident section, once the disk cache holds
// a copy of it as it is now.
void LodStoreMarkSaved(LodStore *Store, int Level, int X, int Y, int Z);

// Sets the hook called for unsaved sections on eviction; NULL clears it.
void LodStoreSetEvictHook(LodStore *Store, LodEvictHook Hook, void *User);

void LodStoreGetStats(const LodStore *Store, LodStoreStats *Out);

// Content hash of every section of one level, in coordinate order, so equal
// content gives an equal hash whatever order it was written in. Zero when the
// level is empty or the sort buffer cannot be allocated.
uint64_t LodStoreLevelHash(const LodStore *Store, int Level);

// The same over part of the level: sections holding no real cell, or the
// ones holding some.
typedef enum {
  LOD_HASH_ALL = 0,
  LOD_HASH_SYNTHETIC_ONLY,
  LOD_HASH_WITH_REAL
} LodHashFilter;
uint64_t LodStoreLevelHashOf(const LodStore *Store, int Level,
                             LodHashFilter Filter);

// floor(Value / 2^Shift) for negative values too.
static inline int LodFloorShift(int Value, int Shift) {
  int Divisor = 1 << Shift;
  int Quotient = Value / Divisor;
  return (Value % Divisor < 0) ? Quotient - 1 : Quotient;
}

#endif
