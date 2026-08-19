#ifndef CHUNK_WORKER_H
#define CHUNK_WORKER_H

#include "world/chunk.h"

// One mesher scratch is allocated per worker, so the renderer sizes its pool
// from this. Declared here rather than duplicated as a matching constant, since
// two numbers that must agree eventually disagree.
#define CHUNK_WORKER_THREAD_COUNT 4

// Runs one queued mesh job on a worker thread, using the mesher scratch
// belonging to MesherSlot. Supplied as a callback rather than called directly so
// the world layer keeps no dependency on the renderer: the mesher is the only
// thing that knows what a mesh job contains, and the workers only carry its id.
typedef void (*MeshJobRunner)(int JobId, int MesherSlot);

void InitChunkWorker(void);
void CloseChunkWorker(void);

// Wire the mesher in before any mesh job is queued.
void SetChunkWorkerMeshRunner(MeshJobRunner Runner);

void EnqueueChunkGeneration(Chunk *ChunkVal);

// The job id comes from the renderer, which owns the snapshot the job carries.
// Returns false when the queue is full, leaving the caller to retry later.
bool EnqueueChunkMeshJob(int JobId);

#endif
