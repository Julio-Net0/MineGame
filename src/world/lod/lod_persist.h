#ifndef WORLD_LOD_LOD_PERSIST_H
#define WORLD_LOD_LOD_PERSIST_H

#include "world/lod/lod_cache.h"
#include <stdbool.h>
#include <stdint.h>

// Connects the client's LOD store to the disk cache (lod_cache.h).
//
// Loading: the synthetic fill's jobs read a column from the cache before
// generating it (LodPersistReadColumn, on the workers); a column cached as
// generated whole is not generated again.
//
// Saving never blocks the frame on disk: sections leave the store as copies
// (or, when evicted, with their storage moved out) and the chunk workers write
// them, at the fill's lowest priority. Unsaved sections are swept once a second
// while the world and the fill are idle, within a small copy budget per frame;
// evicted ones are saved as they leave; shutdown writes whatever is left. A
// column's sections always go out in one write, and a column with a save still
// pending is not read back (LodPersistColumnBusy), so a read never sees half a
// column or an older copy.
//
// Main thread only, except LodPersistReadColumn and LodPersistRunJob.

// Jobs ids from this base on, handed to the fill's worker runner, are saves.
enum { LOD_PERSIST_JOB_BASE = 1024 };

// After LodFillInit succeeded and before the workers start. Directory is the
// cache directory, Seed the world's. False (cache off) when it cannot be
// opened.
bool LodPersistInit(const char *Directory, uint64_t Seed);
// After CloseChunkWorker and before the store is freed: writes everything
// unsaved and logs what it took.
void LodPersistShutdown(void);
bool LodPersistEnabled(void);

// Worker side: see LodCacheReadColumn; -1 when the cache is off.
int LodPersistReadColumn(int Level, int X, int Z, LodCacheEntry *Out,
                         int Capacity, bool *Generated);
// Worker side: runs save job JobId (relative to LOD_PERSIST_JOB_BASE).
void LodPersistRunJob(int JobId);

// Whether a save covering the level-Level column (X, Z) is still pending.
bool LodPersistColumnBusy(int Level, int X, int Z);

// A column the fill resolved: from the cache (Hit) or generated.
void LodPersistCountColumn(bool Hit, int SectionsLoaded);

// Once per frame, after LodFillUpdate. WorldBusy and FillIdle gate the sweep.
void LodPersistUpdate(bool WorldBusy, bool FillIdle);

// Nothing unsaved in the store, held or being written (true when off). Walks
// the store, so for scripts, not per frame.
bool LodPersistIdle(void);

typedef struct {
  bool Enabled;
  uint64_t Hits;
  uint64_t Misses;
  uint64_t SectionsLoaded;
  // Sections handed to save jobs, and those not saved (out of memory or a
  // failed write).
  uint64_t SectionsQueued;
  uint64_t SaveFailures;
  int PendingSections;
  LodCacheStats Cache;
} LodPersistStats;

void LodPersistGetStats(LodPersistStats *Out);

#endif
