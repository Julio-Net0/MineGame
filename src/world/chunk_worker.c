#include "world/chunk_worker.h"
#include "world/world.h"
#include "world/worldgen_column_cache.h"
#include "world/feature.h"
#include "persistence/world_save.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/profiler.h"
#include "platform/os_limits.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// A ring holds QUEUE_SIZE - 1 entries: enough for every active chunk at once.
#define QUEUE_SIZE (MAX_ACTIVE_CHUNKS + 1)
// The LOD scheduler keeps a few dozen jobs in flight at most.
#define LOD_QUEUE_SIZE 257
// Above the LOD renderer's mesh job slots.
#define LOD_MESH_QUEUE_SIZE 257

// Meshing and generation share one pool rather than getting one each. The queue
// lock, condition variable and shutdown path already do what mesh work needs,
// and a second pool would contend for the same cores while doubling the
// shutdown surface.
//
// Meshing takes priority over generation. Mesh work cannot saturate the pool:
// it is capped by the renderer's job slots, refilled nearest first once per
// frame, so generation always gets the remaining capacity, and what the player
// is standing next to is meshed without waiting for the horizon to generate.
//
// Generation is queued per worker, by column: every cube of a column goes to
// the same worker, so the column's 2D data is computed once, in that worker's
// column cache, instead of once per worker. A worker whose own queue is empty
// takes the oldest job of another queue rather than sitting idle; that only
// happens when the load is already draining, so affinity holds for the bulk.
//
// Each queue is a FIFO threaded through Chunk.GenNext. Column affinity can route
// every active chunk to one worker, so a queue must be able to hold all of
// them; a link per chunk gives that capacity once instead of once per worker,
// and an enqueue can never be refused.
typedef struct {
  Chunk *Head;
  Chunk *Tail;
} GenerationQueue;

// Chunks whose generation finished, in completion order, for the main thread to
// react to. Without it the main thread found them by walking every chunk slot
// each tick, which at the maximum render distance means touching tens of
// megabytes to discover a handful of chunks. A slot completes at most once per
// enqueue, so the ring cannot fill in practice; if it ever does, Overflowed
// tells the drain to fall back to one full walk instead of losing events.
typedef struct {
  Chunk *Chunks[QUEUE_SIZE];
  int Head;
  int Tail;
  bool Overflowed;
} CompletionRing;

typedef struct ChunkWorkerState {
  GenerationQueue GenerationQueues[CHUNK_WORKER_MAX_THREADS];

  int MeshQueue[QUEUE_SIZE];
  int MeshHead;
  int MeshTail;

  int LodQueue[LOD_QUEUE_SIZE];
  int LodHead;
  int LodTail;

  int LodMeshQueue[LOD_MESH_QUEUE_SIZE];
  int LodMeshHead;
  int LodMeshTail;

  CompletionRing Completions;

  pthread_t WorkerThreads[CHUNK_WORKER_MAX_THREADS];
  int WorkerIndices[CHUNK_WORKER_MAX_THREADS];
  int ThreadCount;
  pthread_mutex_t QueueMutex;
  pthread_cond_t WorkAvail;
  atomic_bool WorkerRunning;
  MeshJobRunner MeshRunner;
  LodJobRunner LodRunner;
  MeshJobRunner LodMeshRunner;
} ChunkWorkerState;

static ChunkWorkerState *GetWorkerState(void) {
  static ChunkWorkerState State = {
    .MeshHead = 0,
    .MeshTail = 0,
    .ThreadCount = 0,
    .WorkerRunning = false,
    .MeshRunner = (MeshJobRunner)0,
    .LodRunner = (LodJobRunner)0,
    .LodMeshRunner = (MeshJobRunner)0
  };
  return &State;
}

// Caller holds QueueMutex. Mesh or generation work: what a shutdown drains.
static bool HasChunkWork(const ChunkWorkerState *State) {
  for (int Idx = 0; Idx < State->ThreadCount; Idx++) {
    if (State->GenerationQueues[Idx].Head != (Chunk *)0) {
      return true;
    }
  }
  return State->MeshHead != State->MeshTail;
}

// Caller holds QueueMutex.
static bool HasWork(const ChunkWorkerState *State) {
  return HasChunkWork(State) || State->LodHead != State->LodTail ||
         State->LodMeshHead != State->LodMeshTail;
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
  for (int Offset = 0; Offset < State->ThreadCount; Offset++) {
    GenerationQueue *Queue =
        &State->GenerationQueues[(WorkerIndex + Offset) % State->ThreadCount];
    Chunk *Job = Queue->Head;
    if (Job != (Chunk *)0) {
      Queue->Head = Job->GenNext;
      if (Queue->Head == (Chunk *)0) {
        Queue->Tail = (Chunk *)0;
      }
      Job->GenNext = (Chunk *)0;
      return Job;
    }
  }
  return (Chunk *)0;
}

// Caller holds QueueMutex, which is also what publishes the chunk's generated
// state to the thread that drains the ring.
static void PushCompletion(ChunkWorkerState *State, Chunk *Done) {
  CompletionRing *Ring = &State->Completions;
  int NextTail = (Ring->Tail + 1) % QUEUE_SIZE;
  if (NextTail == Ring->Head) {
    Ring->Overflowed = true;
    return;
  }
  Ring->Chunks[Ring->Tail] = Done;
  Ring->Tail = NextTail;
}

static void RunGeneration(Chunk *Target, int WorkerIndex) {
  PROFILE_BEGIN(GenerationStart);

  // Every cube of a column shares this entry, so its 2D noise is sampled once
  // per column for all workers rather than once per cube. Held until flora is
  // placed: a shared entry pinned by the handle stays valid throughout.
  ColumnCache *Cache = GetColumnCache(WorkerIndex);
  ColumnHandle Handle;
  const ColumnEntry *Column = ColumnCacheAcquire(
      Cache, Target->ChunkX, Target->ChunkZ, GetWorldSeed(), &Handle);

  // Before the branch, not after it: generation reads the biome map to choose
  // its blocks, and a chunk read back from disk needs one just the same. A
  // single call ahead of both paths is what stops them from drifting apart.
  FillChunkBiomeMap(Target, Column);

  // Trees are procedural, not persisted: stamp them only on the fresh-
  // generation path, never after a disk load, so player edits (a chopped
  // tree) survive and the save format stays untouched.
  if (!LoadChunkFromDisk(Target)) {
    const TerrainGenOptions OPTIONS = {
        .AllowClassification = true,
        .CountClasses = true,
        .DebugDensity = (float *)0,
        .CornerRows = ColumnHandleCornerRows(&Handle)};
    PROFILE_BEGIN(TerrainStart);
    (void)GenerateChunkTerrainEx(Target, Column, &OPTIONS);
    PROFILE_END(TerrainStart, PROFILE_GEN_TERRAIN);

    PROFILE_BEGIN(FeaturesStart);
    PlaceChunkFeatures(Target, Cache, true);
    // A private entry is not pinned: the structure pass looks up neighbouring
    // columns, which may have reused its slot, so it is looked up again.
    if (Handle.Slot == (struct SharedColumnSlot *)0) {
      Column = ColumnCacheLookup(Cache, Target->ChunkX, Target->ChunkZ,
                                 GetWorldSeed());
    }
    PlaceChunkFlora(Target, Column);
    PROFILE_END(FeaturesStart, PROFILE_GEN_FEATURES);
  }
  ColumnCacheRelease(&Handle);

  // Before IsGenerated is published: readers on the main thread rely on the
  // storage being final once they observe it.
  ChunkEndFill(Target);

  PROFILE_END(GenerationStart, PROFILE_WORKER_GENERATION);

  Target->TerrainJustGenerated = true;
  Target->IsGenerated = true;
  Target->IsGenerating = false;
}

static void *WorkerLoop(void *Arg) {
  ChunkWorkerState *State = GetWorkerState();
  int WorkerIndex = (Arg != (void *)0) ? *(const int *)Arg : 0;
  // Reported inside the lock this loop takes anyway for its next job, so a
  // completion costs no extra lock round trip. It is pushed before the wait
  // below, so a worker going idle never sits on one.
  Chunk *Completed = (Chunk *)0;

  for (;;) {
    if (pthread_mutex_lock(&State->QueueMutex) != 0) {
      break;
    }

    if (Completed != (Chunk *)0) {
      PushCompletion(State, Completed);
      Completed = (Chunk *)0;
    }

    while (atomic_load(&State->WorkerRunning) && !HasWork(State)) {
      pthread_cond_wait(&State->WorkAvail, &State->QueueMutex);
    }

    if (!atomic_load(&State->WorkerRunning) && !HasChunkWork(State)) {
      pthread_mutex_unlock(&State->QueueMutex);
      break;
    }

    Chunk *GenerationTarget = (Chunk *)0;
    int MeshJobId = -1;
    int LodMeshJobId = -1;
    int LodJobId = -1;

    // Mesh work first. It is bounded by the renderer's job slots, which the main
    // thread refills nearest first, so it can never saturate the pool the way
    // an unbounded mesh queue could; generation runs in what is left. Taking
    // generation first instead parked the vicinity's mesh jobs behind every
    // other cube of the load: after a teleport at the maximum render distance
    // nothing near the player appeared until the whole disc had generated.
    // LOD section meshes follow the chunks' for the same reason, bounded the
    // same way by the LOD renderer's slots.
    if (State->MeshHead != State->MeshTail) {
      MeshJobId = State->MeshQueue[State->MeshHead];
      State->MeshHead = (State->MeshHead + 1) % QUEUE_SIZE;
    } else if (State->LodMeshHead != State->LodMeshTail) {
      LodMeshJobId = State->LodMeshQueue[State->LodMeshHead];
      State->LodMeshHead = (State->LodMeshHead + 1) % LOD_MESH_QUEUE_SIZE;
    } else {
      GenerationTarget = TakeGenerationJob(State, WorkerIndex);
      if (GenerationTarget == (Chunk *)0 && State->LodHead != State->LodTail) {
        LodJobId = State->LodQueue[State->LodHead];
        State->LodHead = (State->LodHead + 1) % LOD_QUEUE_SIZE;
      }
    }

    MeshJobRunner Runner = State->MeshRunner;
    MeshJobRunner LodMeshRunner = State->LodMeshRunner;
    LodJobRunner LodRunner = State->LodRunner;
    pthread_mutex_unlock(&State->QueueMutex);

    if (GenerationTarget != (Chunk *)0) {
      RunGeneration(GenerationTarget, WorkerIndex);
      Completed = GenerationTarget;
    } else if (MeshJobId >= 0 && Runner != (MeshJobRunner)0) {
      Runner(MeshJobId, WorkerIndex);
    } else if (LodMeshJobId >= 0 && LodMeshRunner != (MeshJobRunner)0) {
      LodMeshRunner(LodMeshJobId, WorkerIndex);
    } else if (LodJobId >= 0 && LodRunner != (LodJobRunner)0) {
      LodRunner(LodJobId);
    }
  }
  return (void *)0;
}

void SetChunkWorkerMeshRunner(MeshJobRunner Runner) {
  GetWorkerState()->MeshRunner = Runner;
}

void SetChunkWorkerLodRunner(LodJobRunner Runner) {
  GetWorkerState()->LodRunner = Runner;
}

void SetChunkWorkerLodMeshRunner(MeshJobRunner Runner) {
  GetWorkerState()->LodMeshRunner = Runner;
}

int ChunkWorkerDefaultThreadCount(void) {
  int Count = OsGetUsableLogicalProcessors() - 1;
  if (Count < 1) {
    return 1;
  }
  return (Count > CHUNK_WORKER_MAX_THREADS) ? CHUNK_WORKER_MAX_THREADS : Count;
}

int ChunkWorkerThreadCount(void) { return GetWorkerState()->ThreadCount; }

void InitChunkWorker(int ThreadCount, int RenderDistance) {
  ChunkWorkerState *State = GetWorkerState();
  int ClampedCount = ThreadCount;
  if (ClampedCount < 1) {
    ClampedCount = 1;
  } else if (ClampedCount > CHUNK_WORKER_MAX_THREADS) {
    ClampedCount = CHUNK_WORKER_MAX_THREADS;
  }

  // Each worker needs a column cache of its own; with fewer caches than asked
  // for, fewer workers start rather than two sharing one.
  int CacheCount = ColumnCacheInitWorkers(ClampedCount, RenderDistance);
  if (CacheCount < ClampedCount) {
    LogWarn("WORKER: column caches for %d of %d workers; starting %d",
            CacheCount, ClampedCount, CacheCount);
    ClampedCount = CacheCount;
  }

  State->ThreadCount = ClampedCount;
  for (int Idx = 0; Idx < CHUNK_WORKER_MAX_THREADS; Idx++) {
    State->GenerationQueues[Idx].Head = (Chunk *)0;
    State->GenerationQueues[Idx].Tail = (Chunk *)0;
  }
  State->LodHead = 0;
  State->LodTail = 0;
  State->LodMeshHead = 0;
  State->LodMeshTail = 0;
  State->Completions.Head = 0;
  State->Completions.Tail = 0;
  State->Completions.Overflowed = false;
  atomic_store(&State->WorkerRunning, true);
  pthread_mutex_init(&State->QueueMutex, (const pthread_mutexattr_t *)0);
  pthread_cond_init(&State->WorkAvail, (const pthread_condattr_t *)0);
  int Started = 0;
  for (int Idx = 0; Idx < ClampedCount; Idx++) {
    // Each worker owns a mesher scratch, identified by this index. The storage
    // is the state struct's own, so it outlives the thread that reads it.
    State->WorkerIndices[Started] = Started;
    int Err = pthread_create(&State->WorkerThreads[Started],
                             (const pthread_attr_t *)0, WorkerLoop,
                             &State->WorkerIndices[Started]);
    if (Err != 0) {
      LogError("WORKER: pthread_create failed for worker %d (err=%d)", Idx,
               Err);
      break;
    }
    Started++;
  }
  State->ThreadCount = Started;
  if (State->ThreadCount == 0) {
    LogError("WORKER: no chunk worker threads started");
  }
}

void CloseChunkWorker(void) {
  ChunkWorkerState *State = GetWorkerState();
  // Under the lock: a worker that has just seen it still running and found no
  // work is either already waiting (and woken) or has not released the lock
  // yet (and sees it stopped). Without it the broadcast can land between the
  // two, and that worker waits forever.
  pthread_mutex_lock(&State->QueueMutex);
  atomic_store(&State->WorkerRunning, false);
  pthread_cond_broadcast(&State->WorkAvail);
  pthread_mutex_unlock(&State->QueueMutex);
  for (int Idx = 0; Idx < State->ThreadCount; Idx++) {
    pthread_join(State->WorkerThreads[Idx], (void **)0);
  }
  State->ThreadCount = 0;
  pthread_cond_destroy(&State->WorkAvail);
  pthread_mutex_destroy(&State->QueueMutex);
  ColumnCacheFreeWorkers();
}

// Caller holds QueueMutex.
static void AppendGenerationJob(ChunkWorkerState *State, Chunk *ChunkVal) {
  GenerationQueue *Queue = &State->GenerationQueues[ColumnOwner(
      State, ChunkVal->ChunkX, ChunkVal->ChunkZ)];

  ChunkVal->IsGenerating = true;
  ChunkVal->IsGenerated = false;
  ChunkVal->GenNext = (Chunk *)0;
  if (Queue->Tail != (Chunk *)0) {
    Queue->Tail->GenNext = ChunkVal;
  } else {
    Queue->Head = ChunkVal;
  }
  Queue->Tail = ChunkVal;
}

void EnqueueChunkGenerations(Chunk *const *Chunks, int Count) {
  if (Count <= 0) {
    return;
  }
  ChunkWorkerState *State = GetWorkerState();
  pthread_mutex_lock(&State->QueueMutex);
  for (int Idx = 0; Idx < Count; Idx++) {
    AppendGenerationJob(State, Chunks[Idx]);
  }
  pthread_cond_broadcast(&State->WorkAvail);
  pthread_mutex_unlock(&State->QueueMutex);
}

int DrainChunkCompletions(Chunk **Out, int Capacity, bool *OutOverflowed) {
  ChunkWorkerState *State = GetWorkerState();
  CompletionRing *Ring = &State->Completions;
  int Count = 0;

  pthread_mutex_lock(&State->QueueMutex);
  while (Ring->Head != Ring->Tail && Count < Capacity) {
    Out[Count++] = Ring->Chunks[Ring->Head];
    Ring->Head = (Ring->Head + 1) % QUEUE_SIZE;
  }
  *OutOverflowed = Ring->Overflowed;
  Ring->Overflowed = false;
  pthread_mutex_unlock(&State->QueueMutex);

  return Count;
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

bool EnqueueChunkLodJob(int JobId) {
  ChunkWorkerState *State = GetWorkerState();
  pthread_mutex_lock(&State->QueueMutex);
  int NextTail = (State->LodTail + 1) % LOD_QUEUE_SIZE;
  bool Queued = (NextTail != State->LodHead);
  if (Queued) {
    State->LodQueue[State->LodTail] = JobId;
    State->LodTail = NextTail;
    pthread_cond_signal(&State->WorkAvail);
  }
  pthread_mutex_unlock(&State->QueueMutex);
  return Queued;
}

bool EnqueueChunkLodMeshJob(int JobId) {
  ChunkWorkerState *State = GetWorkerState();
  pthread_mutex_lock(&State->QueueMutex);
  int NextTail = (State->LodMeshTail + 1) % LOD_MESH_QUEUE_SIZE;
  bool Queued = (NextTail != State->LodMeshHead);
  if (Queued) {
    State->LodMeshQueue[State->LodMeshTail] = JobId;
    State->LodMeshTail = NextTail;
    pthread_cond_signal(&State->WorkAvail);
  }
  pthread_mutex_unlock(&State->QueueMutex);
  return Queued;
}
