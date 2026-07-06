#ifndef CHUNK_WORKER_H
#define CHUNK_WORKER_H

#include "world/chunk.h"

void InitChunkWorker(void);
void CloseChunkWorker(void);
void EnqueueChunkGeneration(Chunk *ChunkVal);

#endif
