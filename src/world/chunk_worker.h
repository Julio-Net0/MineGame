#ifndef CHUNK_WORKER_H
#define CHUNK_WORKER_H

#include "world/chunk.h"

// Upper bound on worker threads; it bounds every worker index. Storage that is
// significant per worker is allocated for the actual count instead, so raising
// this costs little on machines that run few workers.
#define CHUNK_WORKER_MAX_THREADS 16

// Worker count to use without a --workers override: every logical processor the
// process may run on except one, left to the main thread, clamped to
// [1, CHUNK_WORKER_MAX_THREADS]. Call after any affinity restriction (--cores).
int ChunkWorkerDefaultThreadCount(void);

// Workers started by InitChunkWorker, 0 before it and after CloseChunkWorker.
int ChunkWorkerThreadCount(void);

// Runs one queued mesh job on a worker thread, using the mesher scratch
// belonging to MesherSlot. Supplied as a callback rather than called directly so
// the world layer keeps no dependency on the renderer: the mesher is the only
// thing that knows what a mesh job contains, and the workers only carry its id.
typedef void (*MeshJobRunner)(int JobId, int MesherSlot);

// RenderDistance sizes the shared column table; see ColumnCacheInitWorkers.
void InitChunkWorker(int ThreadCount, int RenderDistance);
void CloseChunkWorker(void);

// Wire the mesher in before any mesh job is queued.
void SetChunkWorkerMeshRunner(MeshJobRunner Runner);

// Enqueues Count chunks for generation, in order, under one lock and one
// wake-up: a lock and a wake per chunk made idle workers contend with the main
// thread for every job.
void EnqueueChunkGenerations(Chunk *const *Chunks, int Count);

// Main thread: moves up to Capacity chunks whose generation finished since the
// last drain into Out, oldest first, and returns how many. An entry may be
// stale (its slot evicted or reused since), so the caller validates each one.
// OutOverflowed is set when completions were dropped because the ring was full;
// the caller must then rediscover them by walking every slot.
int DrainChunkCompletions(Chunk **Out, int Capacity, bool *OutOverflowed);

// The job id comes from the renderer, which owns the snapshot the job carries.
// Returns false when the queue is full, leaving the caller to retry later.
bool EnqueueChunkMeshJob(int JobId);

#endif
