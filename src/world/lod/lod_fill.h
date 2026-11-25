#ifndef WORLD_LOD_LOD_FILL_H
#define WORLD_LOD_LOD_FILL_H

#include <stdbool.h>
#include <stdint.h>

// Fills the client's LOD store with synthetic terrain (lod_gen.h) out to the
// LOD distance, so the horizon exists before, and far beyond, any chunk is
// loaded. Real chunk data keeps precedence cell by cell.
//
// Columns are chosen where the LOD renderer will want them: every level-7
// column within the distance, and every finer column whose parent the camera
// is close enough to refine (or whose parent touches the 1:1 disc). Level 7
// fills first, nearest first, so the whole horizon appears early; every other
// column follows by its distance over its voxel width, the screen size of the
// parent drawn while it is missing, so the terrain right beyond the 1:1 chunks
// refines before far coarse columns, and a parent always precedes its children.
// Columns under the 1:1 chunks are generated too, for the heights the 1:1
// window leaves unloaded; real data keeps every cell it holds.
//
// Jobs run on the chunk workers at their lowest priority, only when no mesh
// or generation work is queued; while the world is still loading at most a
// couple are in flight. Results are written into the store on the main
// thread, encoded already, within a small per-frame budget.
//
// Main thread only, except the job runner the workers call.

// After LodIngestInit and before InitChunkWorker; does nothing unless the LOD
// store exists. False when off or out of memory (synthetic fill then stays off).
bool LodFillInit(int DistanceChunks, int WorkerCount);
// After CloseChunkWorker.
void LodFillShutdown(void);
bool LodFillEnabled(void);

// Changes the LOD distance (0 generates nothing more) and the renderer's refine
// factor at runtime; the wanted columns are rebuilt at the next update.
// Columns already generated stay in the store while it holds them.
void LodFillSetView(int DistanceChunks, int RefineWidths);

// Once per frame, after LodIngestUpdate and before LodIngestFlush (which then
// carries up what synthetic writes over real cells queued). The anchor is the
// load centre's chunk; WorldBusy (1:1 mesh work in flight) keeps in-flight jobs
// and the write budget small.
void LodFillUpdate(int ChunkX, int ChunkZ, int RenderDistance, bool WorldBusy);

// Every wanted column for the current anchor is generated and written (always
// true when off).
bool LodFillIdle(void);

typedef struct {
  bool Enabled;
  int DistanceChunks;
  // Wanted columns not yet dispatched, and jobs running or waiting to be
  // written.
  int Queued;
  int InFlight;
  uint64_t ColumnsGenerated;
  uint64_t SectionsGenerated;
  // Columns taken whole from the disk cache, and their sections.
  uint64_t ColumnsLoaded;
  uint64_t SectionsLoaded;
  // Store writes applied, refused because every cell held real data, and
  // failed (out of range after a move, or out of memory).
  uint64_t SectionsWritten;
  uint64_t SectionsSkipped;
  uint64_t SectionsFailed;
  // Worker time spent generating (and encoding), summed over all workers.
  uint64_t GenMicros;
  // Milliseconds from the start of a fill (the first anchor, or a move once
  // idle) until the horizon level (7) and then every wanted column were in
  // the store; negative until reached.
  double HorizonMs;
  double FillMs;
} LodFillStats;

void LodFillGetStats(LodFillStats *Out);

#endif
