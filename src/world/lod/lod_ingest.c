#include "world/lod/lod_ingest.h"

#include "core/log.h"
#include "core/profiler.h"
#include "platform/os_time.h"

#include <stdlib.h>
#include <string.h>

enum {
  // Level-1 sections span two chunks; the margin keeps chunks at the very edge
  // of the loaded disc inside level 1 so their data reaches the levels above.
  LEVEL1_RADIUS_MARGIN = 2,
  LEVEL1_CHUNKS_PER_SECTION = 2,
  UNIFORM_QUEUE_INITIAL = 256,
  // Patches written to level 1 per batch: the frame's mesh uploads queue up to
  // this many before they are written, and queued uniform chunks are expanded
  // into patches this many at a time. 1 MB of patches, allocated once.
  BATCH_CAPACITY = 512,
  HASH_COMBINE_SHIFT = 7,
  // Pending sections propagated between clock reads.
  FLUSH_BATCH = 8,
  // Spare dense buffers kept once everything is compacted (64 KB).
  IDLE_POOL_KEEP = 4
};

// Main-thread time a frame may spend carrying changes up the levels and, with
// what is left, compacting hot sections; the rest waits for the next frame.
// Tight while the world is still loading, larger once it is idle.
#define FLUSH_BUDGET_BUSY_SECONDS 0.001
#define FLUSH_BUDGET_IDLE_SECONDS 0.004

typedef struct {
  int ChunkX;
  int ChunkY;
  int ChunkZ;
  BlockID BlockId;
  unsigned char BiomeMap[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK]
                        [BIOME_CELLS_PER_CHUNK];
} QueuedUniform;

typedef struct {
  bool Enabled;
  LodStore Store;
  bool HasAnchor;
  int AnchorX;
  int AnchorY;
  int AnchorZ;
  int RenderDistance;
  uint64_t IngestedChunks;
  uint64_t UniformChunks;
  uint64_t OutOfRange;
  uint64_t Failed;

  // Level-1 writes waiting for the frame's batch.
  LodPatch8 *BatchPatches;
  LodChunkWrite *BatchWrites;
  int BatchCount;

  QueuedUniform *Queue;
  int QueueCount;
  int QueueCapacity;
} LodIngestState;

static LodIngestState *GetIngestState(void) {
  static LodIngestState State;
  return &State;
}

// Every level keeps the default radius; level 1 grows with the render distance
// so every chunk the world can load still lands in it.
static void ConfigForDistance(int RenderDistance, LodStoreConfig *Config) {
  LodStoreDefaultConfig(Config);
  int Level1 =
      (RenderDistance / LEVEL1_CHUNKS_PER_SECTION) + LEVEL1_RADIUS_MARGIN;
  if (Level1 > Config->Radius[1]) {
    Config->Radius[1] = Level1;
  }
}

static void FreeIngestState(LodIngestState *State) {
  if (State->Enabled) {
    LodStoreFree(&State->Store);
  }
  free(State->BatchPatches);
  free(State->BatchWrites);
  free(State->Queue);
  *State = (LodIngestState){0};
}

bool LodIngestInit(int RenderDistance) {
  LodIngestState *State = GetIngestState();
  *State = (LodIngestState){0};
  LodStoreConfig Config;
  ConfigForDistance(RenderDistance, &Config);
  State->BatchPatches =
      (LodPatch8 *)malloc(sizeof(LodPatch8) * (size_t)BATCH_CAPACITY);
  State->BatchWrites =
      (LodChunkWrite *)malloc(sizeof(LodChunkWrite) * (size_t)BATCH_CAPACITY);
  bool StoreReady = LodStoreInit(&State->Store, &Config);
  State->Enabled = StoreReady;
  if (!StoreReady || State->BatchPatches == NULL || State->BatchWrites == NULL) {
    FreeIngestState(State);
    LogWarn("LOD: could not allocate the LOD store; LOD stays off");
    return false;
  }
  State->RenderDistance = RenderDistance;
  LogInfo("LOD: ingestion enabled (level-1 radius %d sections, others %d)",
          Config.Radius[1], Config.Radius[2]);
  return true;
}

void LodIngestShutdown(void) { FreeIngestState(GetIngestState()); }

bool LodIngestEnabled(void) { return GetIngestState()->Enabled; }

void LodIngestUpdate(int ChunkX, int ChunkY, int ChunkZ, int RenderDistance) {
  LodIngestState *State = GetIngestState();
  if (!State->Enabled) {
    return;
  }
  bool Moved = !State->HasAnchor || ChunkX != State->AnchorX ||
               ChunkY != State->AnchorY || ChunkZ != State->AnchorZ;
  bool Resized = RenderDistance != State->RenderDistance;
  if (!Moved && !Resized) {
    return;
  }
  PROFILE_BEGIN(IngestStart);
  if (Resized) {
    LodStoreConfig Config;
    ConfigForDistance(RenderDistance, &Config);
    LodStoreSetConfig(&State->Store, &Config);
    State->RenderDistance = RenderDistance;
  }
  if (Moved) {
    LodStoreSetAnchor(&State->Store, ChunkX, ChunkY, ChunkZ);
    State->HasAnchor = true;
    State->AnchorX = ChunkX;
    State->AnchorY = ChunkY;
    State->AnchorZ = ChunkZ;
  }
  PROFILE_END(IngestStart, PROFILE_LOD_INGEST);
}

// Writes the batched patches into level 1, one decode and encode per section.
static void WriteBatch(LodIngestState *State) {
  if (State->BatchCount == 0) {
    return;
  }
  LodBatchResult Result;
  LodStoreWriteChunksDeferred(&State->Store, State->BatchWrites,
                              State->BatchCount, &Result);
  State->IngestedChunks += (uint64_t)State->BatchCount;
  State->OutOfRange += (uint64_t)Result.OutOfRange;
  State->Failed += (uint64_t)Result.Failed;
  State->BatchCount = 0;
}

// The next free batch slot, writing the batch first when it is full.
static int ReserveBatchSlot(LodIngestState *State, int ChunkX, int ChunkY,
                            int ChunkZ) {
  if (State->BatchCount == BATCH_CAPACITY) {
    WriteBatch(State);
  }
  int Slot = State->BatchCount++;
  State->BatchWrites[Slot] = (LodChunkWrite){.ChunkX = ChunkX,
                                             .ChunkY = ChunkY,
                                             .ChunkZ = ChunkZ,
                                             .Sequence = Slot,
                                             .Patch = &State->BatchPatches[Slot]};
  return Slot;
}

void LodIngestChunkPatch(int ChunkX, int ChunkY, int ChunkZ,
                         const LodPatch8 *Patch) {
  LodIngestState *State = GetIngestState();
  if (!State->Enabled) {
    return;
  }
  PROFILE_BEGIN(IngestStart);
  int Slot = ReserveBatchSlot(State, ChunkX, ChunkY, ChunkZ);
  State->BatchPatches[Slot] = *Patch;
  PROFILE_END(IngestStart, PROFILE_LOD_INGEST);
}

void LodIngestUniformChunk(const Chunk *ChunkVal) {
  LodIngestState *State = GetIngestState();
  if (!State->Enabled) {
    return;
  }
  // All air, whatever storage form an edit left it in, or one block id.
  BlockID BlockId = BLOCK_ID_AIR;
  if (ChunkVal->SolidBlockCount != 0) {
    if (!ChunkVal->IsUniform) {
      return;
    }
    BlockId = ChunkVal->UniformBlockId;
  }
  if (State->QueueCount == State->QueueCapacity) {
    int Capacity = (State->QueueCapacity > 0) ? State->QueueCapacity * 2
                                              : UNIFORM_QUEUE_INITIAL;
    QueuedUniform *Grown = (QueuedUniform *)realloc(
        State->Queue, (size_t)Capacity * sizeof(QueuedUniform));
    if (Grown == NULL) {
      State->Failed++;
      return;
    }
    State->Queue = Grown;
    State->QueueCapacity = Capacity;
  }
  QueuedUniform *Entry = &State->Queue[State->QueueCount++];
  Entry->ChunkX = ChunkVal->ChunkX;
  Entry->ChunkY = ChunkVal->ChunkY;
  Entry->ChunkZ = ChunkVal->ChunkZ;
  Entry->BlockId = BlockId;
  memcpy(Entry->BiomeMap, ChunkVal->BiomeMap, sizeof(Entry->BiomeMap));
}

void LodIngestFlush(bool WorldBusy) {
  LodIngestState *State = GetIngestState();
  if (!State->Enabled ||
      (State->BatchCount == 0 && State->QueueCount == 0 &&
       State->Store.PendingCount == 0 && State->Store.HotLive == 0)) {
    return;
  }
  PROFILE_BEGIN(IngestStart);
  // One budget for the whole flush: the level-1 writes below are never
  // deferred (order and memory), so propagation gets what they leave.
  double Deadline = OsGetTimeSeconds() + (WorldBusy ? FLUSH_BUDGET_BUSY_SECONDS
                                                    : FLUSH_BUDGET_IDLE_SECONDS);
  // The frame's mesh uploads first, then the chunks the mesher resolved
  // without a job, which are the newer state of any chunk in both.
  WriteBatch(State);
  for (int Idx = 0; Idx < State->QueueCount; Idx++) {
    const QueuedUniform *Entry = &State->Queue[Idx];
    int Slot =
        ReserveBatchSlot(State, Entry->ChunkX, Entry->ChunkY, Entry->ChunkZ);
    LodDownsampleUniformChunk(Entry->BlockId, Entry->BiomeMap,
                              &State->BatchPatches[Slot]);
  }
  State->UniformChunks += (uint64_t)State->QueueCount;
  State->QueueCount = 0;
  WriteBatch(State);

  // Propagation first, at least one batch per frame so the backlog always
  // shrinks; compaction only with the time left, since a section compacted
  // while writes still reach it would just turn hot again.
  if (State->Store.PendingCount > 0) {
    do {
      if (!LodStoreFlushSome(&State->Store, FLUSH_BATCH)) {
        State->Failed++;
      }
    } while (State->Store.PendingCount > 0 && OsGetTimeSeconds() < Deadline);
  }
  while (State->Store.PendingCount == 0 && State->Store.HotLive > 0 &&
         OsGetTimeSeconds() < Deadline) {
    if (!LodStoreCompactSome(&State->Store, FLUSH_BATCH)) {
      State->Failed++;
      break;
    }
  }
  if (State->Store.PendingCount == 0 && State->Store.HotLive == 0) {
    LodStoreTrimPool(&State->Store, IDLE_POOL_KEEP);
  }
  PROFILE_END(IngestStart, PROFILE_LOD_INGEST);
}

bool LodIngestIdle(void) {
  const LodIngestState *State = GetIngestState();
  return !State->Enabled ||
         (State->BatchCount == 0 && State->QueueCount == 0 &&
          State->Store.PendingCount == 0 && State->Store.HotLive == 0);
}

int LodIngestPendingCount(void) {
  const LodIngestState *State = GetIngestState();
  return State->Enabled ? State->BatchCount + State->QueueCount +
                              State->Store.PendingCount + State->Store.HotLive
                        : 0;
}

void LodIngestGetStats(LodIngestStats *Out) {
  const LodIngestState *State = GetIngestState();
  *Out = (LodIngestStats){0};
  Out->Enabled = State->Enabled;
  if (!State->Enabled) {
    return;
  }
  LodStoreGetStats(&State->Store, &Out->Store);
  Out->IngestedChunks = State->IngestedChunks;
  Out->UniformChunks = State->UniformChunks;
  Out->OutOfRange = State->OutOfRange;
  Out->Failed = State->Failed;
  Out->TotalBytes = LodIngestBytes();
  Out->BufferBytes = Out->TotalBytes - Out->Store.TotalBytes;
}

uint64_t LodIngestContentHash(void) {
  const LodIngestState *State = GetIngestState();
  if (!State->Enabled) {
    return 0;
  }
  uint64_t Hash = 0;
  for (int Level = 1; Level <= LOD_MAX_LEVEL; Level++) {
    Hash = (Hash << (unsigned)HASH_COMBINE_SHIFT) ^
           (Hash >> (unsigned)HASH_COMBINE_SHIFT) ^
           LodStoreLevelHash(&State->Store, Level);
  }
  return Hash;
}

int LodIngestSectionCount(void) {
  const LodIngestState *State = GetIngestState();
  return State->Enabled ? State->Store.Count : 0;
}

size_t LodIngestBytes(void) {
  const LodIngestState *State = GetIngestState();
  if (!State->Enabled) {
    return 0U;
  }
  return LodStoreBytes(&State->Store) +
         ((sizeof(LodPatch8) + sizeof(LodChunkWrite)) * (size_t)BATCH_CAPACITY) +
         ((size_t)State->QueueCapacity * sizeof(QueuedUniform));
}

uint64_t LodIngestChunkCount(void) { return GetIngestState()->IngestedChunks; }
