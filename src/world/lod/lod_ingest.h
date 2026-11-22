#ifndef WORLD_LOD_LOD_INGEST_H
#define WORLD_LOD_LOD_INGEST_H

#include "world/chunk.h"
#include "world/lod/lod_downsample.h"
#include "world/lod/lod_store.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The client's LOD hierarchy and the one place chunks enter it.
//
// Patches are computed off-thread (the mesh workers downsample the snapshot
// they already hold); every call here runs on the main thread, which owns the
// store, so the store needs no lock. Chunks land in level 1 as they arrive;
// LodIngestFlush, once per frame, carries the frame's changes up the levels,
// rebuilding each changed section once however many chunks touched it.
//
// Nothing is allocated unless LodIngestInit succeeds, and the client calls it
// only when the device can draw LOD, so a run without LOD support pays
// nothing. The dedicated server never calls it.

typedef struct {
  bool Enabled;
  LodStoreStats Store;
  // Chunk patches handed to the store, how many of them came through the
  // uniform fast path (chunks the mesher skips), and how many the clipmap or
  // memory refused.
  uint64_t IngestedChunks;
  uint64_t UniformChunks;
  uint64_t OutOfRange;
  uint64_t Failed;
  // The store plus the ingestion batch and queue buffers.
  size_t BufferBytes;
  size_t TotalBytes;
} LodIngestStats;

// Allocates the store sized for RenderDistance. False (and LOD stays off) when
// allocation fails. Main thread, before the workers start.
bool LodIngestInit(int RenderDistance);
void LodIngestShutdown(void);

// Fixed once the workers start, so workers may read it without a lock.
bool LodIngestEnabled(void);

// Once per frame: re-anchors the clipmap when the load centre crosses into
// another chunk and resizes level 1 when the render distance changed.
void LodIngestUpdate(int ChunkX, int ChunkY, int ChunkZ, int RenderDistance);

// A finished chunk's patch, from its mesh job.
void LodIngestChunkPatch(int ChunkX, int ChunkY, int ChunkZ,
                         const LodPatch8 *Patch);

// A chunk resolved without a mesh job: all air, or one uniform block. Queued
// (a few dozen bytes) and written at the next flush, after the frame's mesh
// uploads, so a chunk emptied after its last mesh keeps its newest state.
void LodIngestUniformChunk(const Chunk *ChunkVal);

// Once per frame, after mesh uploads and queueing: writes the queued uniform
// chunks, then propagates pending changes upward for at most a small time
// budget; what is left waits for the next frame. WorldBusy (mesh work still in
// flight) keeps the budget tight so loading is not slowed; once the world is
// idle the backlog drains with a larger one.
void LodIngestFlush(bool WorldBusy);

// Nothing queued, waiting to propagate or waiting to be compacted (always
// true when off).
bool LodIngestIdle(void);
int LodIngestPendingCount(void);

// Full statistics (walks the store).
void LodIngestGetStats(LodIngestStats *Out);

// Content hash of every level combined, independent of ingestion order; zero
// when off. Walks and sorts the store, so for state dumps, not per frame.
uint64_t LodIngestContentHash(void);

// Cheap figures for a per-frame overlay.
int LodIngestSectionCount(void);
size_t LodIngestBytes(void);
uint64_t LodIngestChunkCount(void);

#endif
