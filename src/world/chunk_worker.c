#include "world/chunk_worker.h"
#include "persistence/world_save.h"
#include <pthread.h>
#include <raylib.h>
#include <stdatomic.h>
#include <stdbool.h>

#define QUEUE_SIZE 2048
#define WORKER_THREAD_COUNT 4

typedef struct ChunkWorkerState {
  Chunk *GenerationQueue[QUEUE_SIZE];
  int QueueHead;
  int QueueTail;
  pthread_t WorkerThreads[WORKER_THREAD_COUNT];
  pthread_mutex_t QueueMutex;
  pthread_cond_t WorkAvail;
  atomic_bool WorkerRunning;
} ChunkWorkerState;

static ChunkWorkerState *GetWorkerState(void) {
  static ChunkWorkerState State = {
    .QueueHead = 0,
    .QueueTail = 0,
    .WorkerRunning = false
  };
  return &State;
}

static void *WorkerLoop(void *Arg) {
  (void)Arg;
  ChunkWorkerState *State = GetWorkerState();
  for (;;) {
    int LockRes = pthread_mutex_lock(&State->QueueMutex);
    if (LockRes != 0) {
      break;
    }

    while (atomic_load(&State->WorkerRunning) && State->QueueHead == State->QueueTail) {
      pthread_cond_wait(&State->WorkAvail, &State->QueueMutex);
    }

    if (!atomic_load(&State->WorkerRunning) && State->QueueHead == State->QueueTail) {
      pthread_mutex_unlock(&State->QueueMutex);
      break;
    }

    Chunk *Target = State->GenerationQueue[State->QueueHead];
    State->QueueHead = (State->QueueHead + 1) % QUEUE_SIZE;

    pthread_mutex_unlock(&State->QueueMutex);

    if (!LoadChunkFromDisk(Target)) {
      GenerateChunkTerrain(Target);
    }

    Target->TerrainJustGenerated = true;
    Target->IsGenerated = true;
    Target->IsGenerating = false;
  }
  return (void *)0;
}

void InitChunkWorker(void) {
  ChunkWorkerState *State = GetWorkerState();
  atomic_store(&State->WorkerRunning, true);
  pthread_mutex_init(&State->QueueMutex, (const pthread_mutexattr_t *)0);
  pthread_cond_init(&State->WorkAvail, (const pthread_condattr_t *)0);
  #pragma unroll 4
  for (int Idx = 0; Idx < WORKER_THREAD_COUNT; Idx++) {
    pthread_create(&State->WorkerThreads[Idx], (const pthread_attr_t *)0, WorkerLoop, (void *)0);
  }
}

void CloseChunkWorker(void) {
  ChunkWorkerState *State = GetWorkerState();
  atomic_store(&State->WorkerRunning, false);
  pthread_cond_broadcast(&State->WorkAvail);
  #pragma unroll 4
  for (int Idx = 0; Idx < WORKER_THREAD_COUNT; Idx++) {
    pthread_join(State->WorkerThreads[Idx], (void **)0);
  }
  pthread_cond_destroy(&State->WorkAvail);
  pthread_mutex_destroy(&State->QueueMutex);
}

void EnqueueChunkGeneration(Chunk *ChunkVal) {
  ChunkWorkerState *State = GetWorkerState();
  pthread_mutex_lock(&State->QueueMutex);

  int NextTail = (State->QueueTail + 1) % QUEUE_SIZE;

  if (NextTail != State->QueueHead) {
    ChunkVal->IsGenerating = true;
    ChunkVal->IsGenerated = false;

    State->GenerationQueue[State->QueueTail] = ChunkVal;
    State->QueueTail = NextTail;
    pthread_cond_signal(&State->WorkAvail);
  } else {
    TraceLog(LOG_WARNING, "Chunk generation queue full! Chunk rejected at: %d, %d, %d", ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
    ChunkVal->IsGenerating = false;
    ChunkVal->IsGenerated = false;
  }

  pthread_mutex_unlock(&State->QueueMutex);
}
