#include "world/chunk_worker.h"
#include "world/worldgen_column_cache.h"
#include "world/feature.h"
#include "persistence/world_save.h"
#include "core/hash.h"
#include "core/profiler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define QUEUE_SIZE 2048

// Meshing and generation share one pool rather than getting one each. The queue
// lock, condition variable and shutdown path already do what mesh work needs,
// and a second pool would contend for the same cores while doubling the
// shutdown surface.
//
// Generation takes priority over meshing: a chunk cannot be meshed until its
// neighbours are generated, so a pool saturated with mesh work would stall the
// generation that unblocks it.
//
// Generation is queued per worker, by column: every cube of a column goes to
// the same worker, so the column's 2D data is computed once, in that worker's
// column cache, instead of once per worker. A worker whose own queue is empty
// takes the oldest job of another queue rather than sitting idle; that only
// happens when the load is already draining, so affinity holds for the bulk.
typedef struct {
  Chunk *Jobs[QUEUE_SIZE];
  int Head;
  int Tail;
} GenerationQueue;

typedef struct ChunkWorkerState {
  GenerationQueue GenerationQueues[CHUNK_WORKER_MAX_THREADS];

  int MeshQueue[QUEUE_SIZE];
  int MeshHead;
  int MeshTail;

  pthread_t WorkerThreads[CHUNK_WORKER_MAX_THREADS];
  int WorkerIndices[CHUNK_WORKER_MAX_THREADS];
  int ThreadCount;
  pthread_mutex_t QueueMutex;
  pthread_cond_t WorkAvail;
  atomic_bool WorkerRunning;
  MeshJobRunner MeshRunner;
} ChunkWorkerState;

static ChunkWorkerState *GetWorkerState(void) {
  static ChunkWorkerState State = {
    .MeshHead = 0,
    .MeshTail = 0,
    .ThreadCount = 0,
    .WorkerRunning = false,
    .MeshRunner = (MeshJobRunner)0
  };
  return &State;
}

static bool QueueIsEmpty(const GenerationQueue *Queue) {
  return Queue->Head == Queue->Tail;
}

// Caller holds QueueMutex.
static bool HasWork(const ChunkWorkerState *State) {
  for (int Idx = 0; Idx < CHUNK_WORKER_MAX_THREADS; Idx++) {
    if (!QueueIsEmpty(&State->GenerationQueues[Idx])) {
      return true;
    }
  }
  return State->MeshHead != State->MeshTail;
}

// The worker that owns a column. Columns are grouped into OWNER_BLOCK x
// OWNER_BLOCK blocks with one owner each: the structure pass reads the 3x3
// columns around a cube, and with per-column owners those neighbours would
// mostly belong to other workers, each recomputing them in its own cache.
// Blocks are hashed rather than striped so the load spreads over all workers.
static int ColumnOwner(const ChunkWorkerState *State, int ChunkX, int ChunkZ) {
  const int OWNER_BLOCK = 4;
  const unsigned int HALF_BITS = 32U;
  int Count = (State->ThreadCount > 0) ? State->ThreadCount : 1;
  int BlockX = (ChunkX >= 0) ? ChunkX / OWNER_BLOCK
                             : ((ChunkX + 1) / OWNER_BLOCK) - 1;
  int BlockZ = (ChunkZ >= 0) ? ChunkZ / OWNER_BLOCK
                             : ((ChunkZ + 1) / OWNER_BLOCK) - 1;
  uint64_t Packed = ((uint64_t)(uint32_t)BlockX << HALF_BITS) |
                    (uint64_t)(uint32_t)BlockZ;
  return (int)(SplitMix64(Packed) % (uint64_t)Count);
}

// Caller holds QueueMutex. Own queue first; otherwise the oldest job of the
// next non-empty queue.
static Chunk *TakeGenerationJob(ChunkWorkerState *State, int WorkerIndex) {
  for (int Offset = 0; Offset < CHUNK_WORKER_MAX_THREADS; Offset++) {
    GenerationQueue *Queue =
        &State->GenerationQueues[(WorkerIndex + Offset) % CHUNK_WORKER_MAX_THREADS];
    if (!QueueIsEmpty(Queue)) {
      Chunk *Job = Queue->Jobs[Queue->Head];
      Queue->Head = (Queue->Head + 1) % QUEUE_SIZE;
      return Job;
    }
  }
  return (Chunk *)0;
}

static void RunGeneration(Chunk *Target, int WorkerIndex) {
  PROFILE_BEGIN(GenerationStart);

  // Every cube of a column shares this entry, so its 2D noise is sampled once
  // per column on this worker rather than once per cube.
  const ColumnEntry *Column =
      ColumnCacheLookup(GetColumnCache(WorkerIndex), Target->ChunkX,
                        Target->ChunkZ, GetWorldSeed());

  // Before the branch, not after it: generation reads the biome map to choose
  // its blocks, and a chunk read back from disk needs one just the same. A
  // single call ahead of both paths is what stops them from drifting apart.
  FillChunkBiomeMap(Target, Column);

  // Trees are procedural, not persisted: stamp them only on the fresh-
  // generation path, never after a disk load, so player edits (a chopped
  // tree) survive and the save format stays untouched.
  if (!LoadChunkFromDisk(Target)) {
    const TerrainGenOptions OPTIONS = {.AllowClassification = true,
                                       .CountClasses = true};
    PROFILE_BEGIN(TerrainStart);
    (void)GenerateChunkTerrainEx(Target, Column, &OPTIONS);
    PROFILE_END(TerrainStart, PROFILE_GEN_TERRAIN);

    PROFILE_BEGIN(FeaturesStart);
    ColumnCache *Cache = GetColumnCache(WorkerIndex);
    PlaceChunkFeatures(Target, Cache, true);
    // The structure pass looks up neighbouring columns, which may have reused
    // this column's slot, so the entry is looked up again for flora.
    Column = ColumnCacheLookup(Cache, Target->ChunkX, Target->ChunkZ,
                               GetWorldSeed());
    PlaceChunkFlora(Target, Column);
    PROFILE_END(FeaturesStart, PROFILE_GEN_FEATURES);
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
    GenerationTarget = TakeGenerationJob(State, WorkerIndex);
    if (GenerationTarget == (Chunk *)0) {
      MeshJobId = State->MeshQueue[State->MeshHead];
      State->MeshHead = (State->MeshHead + 1) % QUEUE_SIZE;
    }

    MeshJobRunner Runner = State->MeshRunner;
    pthread_mutex_unlock(&State->QueueMutex);

    if (GenerationTarget != (Chunk *)0) {
      RunGeneration(GenerationTarget, WorkerIndex);
    } else if (MeshJobId >= 0 && Runner != (MeshJobRunner)0) {
      Runner(MeshJobId, WorkerIndex);
    }
  }
  return (void *)0;
}

void SetChunkWorkerMeshRunner(MeshJobRunner Runner) {
  GetWorkerState()->MeshRunner = Runner;
}

void InitChunkWorker(int ThreadCount) {
  ChunkWorkerState *State = GetWorkerState();
  int ClampedCount = ThreadCount;
  if (ClampedCount < 1) {
    ClampedCount = 1;
  } else if (ClampedCount > CHUNK_WORKER_MAX_THREADS) {
    ClampedCount = CHUNK_WORKER_MAX_THREADS;
  }

  State->ThreadCount = ClampedCount;
  atomic_store(&State->WorkerRunning, true);
  pthread_mutex_init(&State->QueueMutex, (const pthread_mutexattr_t *)0);
  pthread_cond_init(&State->WorkAvail, (const pthread_condattr_t *)0);
  for (int Idx = 0; Idx < State->ThreadCount; Idx++) {
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
  for (int Idx = 0; Idx < State->ThreadCount; Idx++) {
    pthread_join(State->WorkerThreads[Idx], (void **)0);
  }
  State->ThreadCount = 0;
  pthread_cond_destroy(&State->WorkAvail);
  pthread_mutex_destroy(&State->QueueMutex);
}

void EnqueueChunkGeneration(Chunk *ChunkVal) {
  ChunkWorkerState *State = GetWorkerState();
  pthread_mutex_lock(&State->QueueMutex);

  GenerationQueue *Queue = &State->GenerationQueues[ColumnOwner(
      State, ChunkVal->ChunkX, ChunkVal->ChunkZ)];
  int NextTail = (Queue->Tail + 1) % QUEUE_SIZE;

  if (NextTail != Queue->Head) {
    ChunkVal->IsGenerating = true;
    ChunkVal->IsGenerated = false;

    Queue->Jobs[Queue->Tail] = ChunkVal;
    Queue->Tail = NextTail;
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
