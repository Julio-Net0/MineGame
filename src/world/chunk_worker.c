#include "world/chunk_worker.h"
#include "world/feature.h"
#include "persistence/world_save.h"
#include "core/profiler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

#define QUEUE_SIZE 2048

// Meshing and generation share one pool rather than getting one each. The queue,
// condition variable and shutdown path already do what mesh work needs, and a
// second pool would contend for the same cores while doubling the shutdown
// surface.
//
// They are separate queues within that pool, because generation takes priority:
// a chunk cannot be meshed until its neighbours are generated, so a pool
// saturated with mesh work would stall the generation that unblocks it.
typedef struct ChunkWorkerState {
  Chunk *GenerationQueue[QUEUE_SIZE];
  int GenerationHead;
  int GenerationTail;

  int MeshQueue[QUEUE_SIZE];
  int MeshHead;
  int MeshTail;

  pthread_t WorkerThreads[CHUNK_WORKER_THREAD_COUNT];
  int WorkerIndices[CHUNK_WORKER_THREAD_COUNT];
  pthread_mutex_t QueueMutex;
  pthread_cond_t WorkAvail;
  atomic_bool WorkerRunning;
  MeshJobRunner MeshRunner;
} ChunkWorkerState;

static ChunkWorkerState *GetWorkerState(void) {
  static ChunkWorkerState State = {
    .GenerationHead = 0,
    .GenerationTail = 0,
    .MeshHead = 0,
    .MeshTail = 0,
    .WorkerRunning = false,
    .MeshRunner = (MeshJobRunner)0
  };
  return &State;
}

// Caller holds QueueMutex.
static bool HasWork(const ChunkWorkerState *State) {
  return (State->GenerationHead != State->GenerationTail) ||
         (State->MeshHead != State->MeshTail);
}

static void RunGeneration(Chunk *Target) {
  PROFILE_BEGIN(GenerationStart);

  // Before the branch, not after it: generation reads the biome map to choose
  // its blocks, and a chunk read back from disk needs one just the same. A
  // single call ahead of both paths is what stops them from drifting apart.
  FillChunkBiomeMap(Target);

  // Trees are procedural, not persisted: stamp them only on the fresh-
  // generation path, never after a disk load, so player edits (a chopped
  // tree) survive and the save format stays untouched.
  if (!LoadChunkFromDisk(Target)) {
    GenerateChunkTerrain(Target);
    PlaceChunkFeatures(Target);
    PlaceChunkFlora(Target);
  }

  PROFILE_END(GenerationStart, PROFILE_WORKER_GENERATION);

  Target->TerrainJustGenerated = true;
  Target->IsGenerated = true;
  Target->IsGenerating = false;
}

static void *WorkerLoop(void *Arg) {
  ChunkWorkerState *State = GetWorkerState();
  int WorkerIndex = (Arg != (void *)0) ? *(const int *)Arg : 0;

  for (;;) {
    if (pthread_mutex_lock(&State->QueueMutex) != 0) {
      break;
    }

    while (atomic_load(&State->WorkerRunning) && !HasWork(State)) {
      pthread_cond_wait(&State->WorkAvail, &State->QueueMutex);
    }

    if (!atomic_load(&State->WorkerRunning) && !HasWork(State)) {
      pthread_mutex_unlock(&State->QueueMutex);
      break;
    }

    Chunk *GenerationTarget = (Chunk *)0;
    int MeshJobId = -1;

    // Generation first, always: meshing depends on it, so draining mesh work
    // ahead of it would stall the pipeline that feeds the mesh queue.
    if (State->GenerationHead != State->GenerationTail) {
      GenerationTarget = State->GenerationQueue[State->GenerationHead];
      State->GenerationHead = (State->GenerationHead + 1) % QUEUE_SIZE;
    } else {
      MeshJobId = State->MeshQueue[State->MeshHead];
      State->MeshHead = (State->MeshHead + 1) % QUEUE_SIZE;
    }

    MeshJobRunner Runner = State->MeshRunner;
    pthread_mutex_unlock(&State->QueueMutex);

    if (GenerationTarget != (Chunk *)0) {
      RunGeneration(GenerationTarget);
    } else if (MeshJobId >= 0 && Runner != (MeshJobRunner)0) {
      Runner(MeshJobId, WorkerIndex);
    }
  }
  return (void *)0;
}

void SetChunkWorkerMeshRunner(MeshJobRunner Runner) {
  GetWorkerState()->MeshRunner = Runner;
}

void InitChunkWorker(void) {
  ChunkWorkerState *State = GetWorkerState();
  atomic_store(&State->WorkerRunning, true);
  pthread_mutex_init(&State->QueueMutex, (const pthread_mutexattr_t *)0);
  pthread_cond_init(&State->WorkAvail, (const pthread_condattr_t *)0);
  for (int Idx = 0; Idx < CHUNK_WORKER_THREAD_COUNT; Idx++) {
    // Each worker owns a mesher scratch, identified by this index. The storage
    // is the state struct's own, so it outlives the thread that reads it.
    State->WorkerIndices[Idx] = Idx;
    pthread_create(&State->WorkerThreads[Idx], (const pthread_attr_t *)0,
                   WorkerLoop, &State->WorkerIndices[Idx]);
  }
}

void CloseChunkWorker(void) {
  ChunkWorkerState *State = GetWorkerState();
  atomic_store(&State->WorkerRunning, false);
  pthread_cond_broadcast(&State->WorkAvail);
  for (int Idx = 0; Idx < CHUNK_WORKER_THREAD_COUNT; Idx++) {
    pthread_join(State->WorkerThreads[Idx], (void **)0);
  }
  pthread_cond_destroy(&State->WorkAvail);
  pthread_mutex_destroy(&State->QueueMutex);
}

void EnqueueChunkGeneration(Chunk *ChunkVal) {
  ChunkWorkerState *State = GetWorkerState();
  pthread_mutex_lock(&State->QueueMutex);

  int NextTail = (State->GenerationTail + 1) % QUEUE_SIZE;

  if (NextTail != State->GenerationHead) {
    ChunkVal->IsGenerating = true;
    ChunkVal->IsGenerated = false;

    State->GenerationQueue[State->GenerationTail] = ChunkVal;
    State->GenerationTail = NextTail;
    pthread_cond_signal(&State->WorkAvail);
  } else {
    (void)fprintf(stderr, "Chunk generation queue full! Chunk rejected at: %d, %d, %d\n", ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
    ChunkVal->IsGenerating = false;
    ChunkVal->IsGenerated = false;
  }

  pthread_mutex_unlock(&State->QueueMutex);
}

bool EnqueueChunkMeshJob(int JobId) {
  ChunkWorkerState *State = GetWorkerState();
  pthread_mutex_lock(&State->QueueMutex);

  int NextTail = (State->MeshTail + 1) % QUEUE_SIZE;
  bool Queued = (NextTail != State->MeshHead);

  if (Queued) {
    State->MeshQueue[State->MeshTail] = JobId;
    State->MeshTail = NextTail;
    pthread_cond_signal(&State->WorkAvail);
  }

  pthread_mutex_unlock(&State->QueueMutex);
  return Queued;
}
