#include "world/lod/lod_persist.h"

#include "core/log.h"
#include "core/profiler.h"
#include "platform/os_time.h"
#include "world/chunk.h"
#include "world/chunk_worker.h"
#include "world/lod/lod_ingest.h"
#include "world/lod/lod_store.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
  SAVE_JOBS = 4,
  SAVE_JOB_ITEMS = 1024,
  LIST_INITIAL = 256,
  // Level, region x and z, column, height.
  KEY_PARTS = 5,
  MS_PER_SECOND = 1000
};

#define SWEEP_INTERVAL_SECONDS 1.0
#define COPY_BUDGET_SECONDS 0.001
#define BYTES_PER_MEGABYTE 1048576.0

typedef enum { SAVE_FREE = 0, SAVE_READY, SAVE_QUEUED, SAVE_DONE } SavePhase;

typedef struct {
  atomic_int Phase;
  int Count;
  bool Passed;
  LodCacheItem Items[SAVE_JOB_ITEMS];
  LodSection Sections[SAVE_JOB_ITEMS];
} SaveJob;

typedef struct {
  LodStoreKey Key;
  // Eviction order: of two held copies of one key the later is newer.
  uint64_t Sequence;
  LodSection Section;
} HeldSection;

typedef struct {
  int Level;
  int RegionX;
  int RegionZ;
} RegionKey;

// Regions some queued or running save job writes. Two copies of one key never
// travel in jobs that could run at once, so a newer copy is always written
// after an older one: a region in flight takes no new job until it lands.
typedef struct {
  RegionKey *Keys;
  int Count;
  int Capacity;
} RegionSet;

typedef struct {
  int Level;
  int RegionX;
  int RegionZ;
  int Count;
} BusyRegion;

typedef struct {
  bool Enabled;
  LodCache *Cache;
  SaveJob *Jobs;
  // Evicted sections waiting for a job.
  HeldSection *Held;
  int HeldCount;
  uint64_t HeldSequence;
  int HeldCapacity;
  // Unsaved keys found by the last sweep, from Cursor on.
  LodStoreKey *Sweep;
  int SweepCursor;
  int SweepCount;
  int SweepCapacity;
  // Regions with held or in-flight sections, counted per section.
  BusyRegion *Busy;
  int BusyCount;
  int BusyCapacity;
  double NextSweep;
  RegionSet InFlight;
  uint64_t Hits;
  uint64_t Misses;
  uint64_t SectionsLoaded;
  uint64_t SectionsQueued;
  uint64_t SaveFailures;
} PersistState;

static PersistState *GetPersistState(void) {
  static PersistState State;
  return &State;
}

static bool Grow(void **Array, int *Capacity, int Needed, size_t Element) {
  if (Needed <= *Capacity) {
    return true;
  }
  int Next = (*Capacity > 0) ? *Capacity : LIST_INITIAL;
  while (Next < Needed) {
    Next *= 2;
  }
  void *Grown = realloc(*Array, (size_t)Next * Element);
  if (Grown == NULL) {
    return false;
  }
  *Array = Grown;
  *Capacity = Next;
  return true;
}

// --- keys and busy regions --------------------------------------------------

static void KeyParts(LodStoreKey Key, int Parts[KEY_PARTS]) {
  int RegionX = LodFloorShift(Key.X, LOD_CACHE_REGION_SHIFT);
  int RegionZ = LodFloorShift(Key.Z, LOD_CACHE_REGION_SHIFT);
  Parts[0] = Key.Level;
  Parts[1] = RegionX;
  Parts[2] = RegionZ;
  Parts[3] = ((Key.X - (RegionX << LOD_CACHE_REGION_SHIFT)) * LOD_CACHE_REGION_COLUMNS) +
             (Key.Z - (RegionZ << LOD_CACHE_REGION_SHIFT));
  Parts[4] = Key.Y;
}

static int CompareKeys(LodStoreKey A, LodStoreKey B) {
  int PartsA[KEY_PARTS];
  int PartsB[KEY_PARTS];
  KeyParts(A, PartsA);
  KeyParts(B, PartsB);
  for (int Part = 0; Part < KEY_PARTS; Part++) {
    if (PartsA[Part] != PartsB[Part]) {
      return (PartsA[Part] < PartsB[Part]) ? -1 : 1;
    }
  }
  return 0;
}

static int CompareStoreKeys(const void *Left, const void *Right) {
  return CompareKeys(*(const LodStoreKey *)Left, *(const LodStoreKey *)Right);
}

static int CompareHeld(const void *Left, const void *Right) {
  const HeldSection *A = (const HeldSection *)Left;
  const HeldSection *B = (const HeldSection *)Right;
  int Order = CompareKeys(A->Key, B->Key);
  if (Order != 0) {
    return Order;
  }
  return (A->Sequence < B->Sequence) ? -1 : (A->Sequence > B->Sequence);
}

static RegionKey RegionOf(LodStoreKey Key) {
  return (RegionKey){.Level = Key.Level,
                     .RegionX = LodFloorShift(Key.X, LOD_CACHE_REGION_SHIFT),
                     .RegionZ = LodFloorShift(Key.Z, LOD_CACHE_REGION_SHIFT)};
}

static bool RegionSetHas(const RegionSet *Set, RegionKey Key) {
  for (int Idx = 0; Idx < Set->Count; Idx++) {
    const RegionKey *Entry = &Set->Keys[Idx];
    if (Entry->Level == Key.Level && Entry->RegionX == Key.RegionX &&
        Entry->RegionZ == Key.RegionZ) {
      return true;
    }
  }
  return false;
}

static bool RegionSetAdd(RegionSet *Set, RegionKey Key) {
  if (RegionSetHas(Set, Key)) {
    return true;
  }
  if (!Grow((void **)&Set->Keys, &Set->Capacity, Set->Count + 1, sizeof(RegionKey))) {
    return false;
  }
  Set->Keys[Set->Count++] = Key;
  return true;
}

static bool SameColumn(LodStoreKey A, LodStoreKey B) {
  return A.Level == B.Level && A.X == B.X && A.Z == B.Z;
}

static int FindBusy(const PersistState *State, int Level, int RegionX, int RegionZ) {
  for (int Idx = 0; Idx < State->BusyCount; Idx++) {
    const BusyRegion *Region = &State->Busy[Idx];
    if (Region->Level == Level && Region->RegionX == RegionX &&
        Region->RegionZ == RegionZ) {
      return Idx;
    }
  }
  return -1;
}

static bool BusyAdd(PersistState *State, LodStoreKey Key) {
  int RegionX = LodFloorShift(Key.X, LOD_CACHE_REGION_SHIFT);
  int RegionZ = LodFloorShift(Key.Z, LOD_CACHE_REGION_SHIFT);
  int Found = FindBusy(State, Key.Level, RegionX, RegionZ);
  if (Found >= 0) {
    State->Busy[Found].Count++;
    return true;
  }
  if (!Grow((void **)&State->Busy, &State->BusyCapacity, State->BusyCount + 1,
            sizeof(BusyRegion))) {
    return false;
  }
  State->Busy[State->BusyCount++] = (BusyRegion){
      .Level = Key.Level, .RegionX = RegionX, .RegionZ = RegionZ, .Count = 1};
  return true;
}

static void BusyRelease(PersistState *State, int Level, int X, int Z) {
  int Found = FindBusy(State, Level, LodFloorShift(X, LOD_CACHE_REGION_SHIFT),
                       LodFloorShift(Z, LOD_CACHE_REGION_SHIFT));
  if (Found >= 0 && --State->Busy[Found].Count == 0) {
    State->Busy[Found] = State->Busy[--State->BusyCount];
  }
}

bool LodPersistColumnBusy(int Level, int X, int Z) {
  const PersistState *State = GetPersistState();
  return State->Enabled &&
         FindBusy(State, Level, LodFloorShift(X, LOD_CACHE_REGION_SHIFT),
                  LodFloorShift(Z, LOD_CACHE_REGION_SHIFT)) >= 0;
}

// --- eviction ---------------------------------------------------------------

static void OnEvict(void *User, LodStoreKey Key, LodSection *Section) {
  PersistState *State = (PersistState *)User;
  if (!Grow((void **)&State->Held, &State->HeldCapacity, State->HeldCount + 1,
            sizeof(HeldSection)) ||
      !BusyAdd(State, Key)) {
    State->SaveFailures++;
    return;
  }
  HeldSection *Held = &State->Held[State->HeldCount++];
  Held->Key = Key;
  Held->Sequence = State->HeldSequence++;
  LodSectionInit(&Held->Section);
  uint64_t Present = Section->PresentCells;
  uint64_t Ingested = Section->IngestedCells;
  bool Generated = Section->GeneratedColumn;
  LodSectionAdoptContent(&Held->Section, Section);
  Held->Section.PresentCells = Present;
  Held->Section.IngestedCells = Ingested;
  Held->Section.GeneratedColumn = Generated;
}

// --- jobs -------------------------------------------------------------------

void LodPersistRunJob(int JobId) {
  PersistState *State = GetPersistState();
  if (JobId < 0 || JobId >= SAVE_JOBS || State->Jobs == NULL) {
    return;
  }
  SaveJob *Job = &State->Jobs[JobId];
  Job->Passed = LodCacheWrite(State->Cache, Job->Items, Job->Count);
  atomic_store_explicit(&Job->Phase, SAVE_DONE, memory_order_release);
}

static void ReapJob(PersistState *State, SaveJob *Job) {
  for (int Idx = 0; Idx < Job->Count; Idx++) {
    BusyRelease(State, Job->Items[Idx].Level, Job->Items[Idx].X, Job->Items[Idx].Z);
    LodSectionFree(&Job->Sections[Idx]);
  }
  if (!Job->Passed) {
    State->SaveFailures += (uint64_t)Job->Count;
  }
  Job->Count = 0;
  atomic_store_explicit(&Job->Phase, SAVE_FREE, memory_order_relaxed);
}

static SaveJob *FreeJob(PersistState *State, int *OutIdx) {
  for (int Idx = 0; Idx < SAVE_JOBS; Idx++) {
    if (atomic_load_explicit(&State->Jobs[Idx].Phase, memory_order_relaxed) ==
        SAVE_FREE) {
      *OutIdx = Idx;
      return &State->Jobs[Idx];
    }
  }
  return NULL;
}

static void AddItem(SaveJob *Job, LodStoreKey Key) {
  Job->Items[Job->Count] = (LodCacheItem){.Level = Key.Level,
                                          .X = Key.X,
                                          .Y = Key.Y,
                                          .Z = Key.Z,
                                          .Section = &Job->Sections[Job->Count]};
  Job->Count++;
}

static void DispatchReady(PersistState *State) {
  for (int Idx = 0; Idx < SAVE_JOBS; Idx++) {
    SaveJob *Job = &State->Jobs[Idx];
    if (atomic_load_explicit(&Job->Phase, memory_order_relaxed) != SAVE_READY) {
      continue;
    }
    atomic_store_explicit(&Job->Phase, SAVE_QUEUED, memory_order_release);
    if (!EnqueueChunkLodJob(LOD_PERSIST_JOB_BASE + Idx)) {
      atomic_store_explicit(&Job->Phase, SAVE_READY, memory_order_relaxed);
      return;
    }
    State->SectionsQueued += (uint64_t)Job->Count;
  }
}

static void JobRegions(const SaveJob *Job, RegionSet *Set) {
  for (int Idx = 0; Idx < Job->Count; Idx++) {
    const LodCacheItem *Item = &Job->Items[Idx];
    LodStoreKey Key = {.X = Item->X, .Y = Item->Y, .Z = Item->Z, .Level = Item->Level};
    (void)RegionSetAdd(Set, RegionOf(Key));
  }
}

static void InFlightRegions(const PersistState *State, RegionSet *Set) {
  Set->Count = 0;
  for (int Idx = 0; State->Jobs != NULL && Idx < SAVE_JOBS; Idx++) {
    if (atomic_load_explicit(&State->Jobs[Idx].Phase, memory_order_relaxed) !=
        SAVE_FREE) {
      JobRegions(&State->Jobs[Idx], Set);
    }
  }
}

// A job with room for Needed more sections: Current while it has room, else a
// free one (Current is then handed off as ready).
static SaveJob *JobWithRoom(PersistState *State, SaveJob *Current, int Needed) {
  if (Current != NULL && Current->Count + Needed <= SAVE_JOB_ITEMS) {
    return Current;
  }
  if (Current != NULL) {
    atomic_store_explicit(&Current->Phase, SAVE_READY, memory_order_relaxed);
  }
  int Idx = 0;
  return FreeJob(State, &Idx);
}

// Held (evicted) sections into jobs, whole columns, skipping regions in
// flight; they own their storage already. Of two held copies of a key only the
// newer is kept. InFlight is refreshed with the jobs made.
static void PackHeld(PersistState *State, RegionSet *InFlight) {
  if (State->HeldCount == 0) {
    return;
  }
  qsort(State->Held, (size_t)State->HeldCount, sizeof(HeldSection), CompareHeld);
  int Kept = 0;
  SaveJob *Job = NULL;
  int Start = 0;
  while (Start < State->HeldCount) {
    int End = Start + 1;
    while (End < State->HeldCount &&
           SameColumn(State->Held[End].Key, State->Held[Start].Key)) {
      End++;
    }
    SaveJob *Target = NULL;
    if (!RegionSetHas(InFlight, RegionOf(State->Held[Start].Key))) {
      Target = JobWithRoom(State, Job, End - Start);
      Job = Target;
    }
    for (int Idx = Start; Idx < End; Idx++) {
      HeldSection *Held = &State->Held[Idx];
      if (Idx + 1 < End && CompareKeys(Held->Key, State->Held[Idx + 1].Key) == 0) {
        BusyRelease(State, Held->Key.Level, Held->Key.X, Held->Key.Z);
        LodSectionFree(&Held->Section);
      } else if (Target != NULL) {
        Target->Sections[Target->Count] = Held->Section;
        AddItem(Target, Held->Key);
      } else {
        State->Held[Kept++] = *Held;
      }
    }
    Start = End;
  }
  State->HeldCount = Kept;
  if (Job != NULL && Job->Count > 0) {
    atomic_store_explicit(&Job->Phase, SAVE_READY, memory_order_relaxed);
  }
  InFlightRegions(State, InFlight);
}

// --- sweep ------------------------------------------------------------------

static bool CollectUnsaved(PersistState *State, const LodStore *Store) {
  State->SweepCount = 0;
  State->SweepCursor = 0;
  for (int Idx = 0; Idx < Store->Capacity; Idx++) {
    const LodStoreSlot *Slot = &Store->Slots[Idx];
    if (Slot->Level <= 0 || !Slot->Section->Unsaved) {
      continue;
    }
    if (!Grow((void **)&State->Sweep, &State->SweepCapacity, State->SweepCount + 1,
              sizeof(LodStoreKey))) {
      return false;
    }
    State->Sweep[State->SweepCount++] = (LodStoreKey){
        .X = Slot->X, .Y = Slot->Y, .Z = Slot->Z, .Level = Slot->Level};
  }
  qsort(State->Sweep, (size_t)State->SweepCount, sizeof(LodStoreKey),
        CompareStoreKeys);
  return true;
}

// Copies swept sections into jobs, a whole column at a time, until the budget
// or the free jobs run out, or the next column's region is in flight (the
// sweep then resumes there next frame). Sections changed again later are swept
// again.
static void CopySwept(PersistState *State, LodStore *Store, double Deadline,
                      const RegionSet *InFlight) {
  SaveJob *Job = NULL;
  while (State->SweepCursor < State->SweepCount && OsGetTimeSeconds() < Deadline) {
    LodStoreKey Column = State->Sweep[State->SweepCursor];
    if (RegionSetHas(InFlight, RegionOf(Column))) {
      break;
    }
    SaveJob *Target = JobWithRoom(State, Job, LOD_CACHE_MAX_COLUMN_SECTIONS);
    Job = Target;
    if (Target == NULL) {
      break;
    }
    while (State->SweepCursor < State->SweepCount &&
           SameColumn(State->Sweep[State->SweepCursor], Column)) {
      LodStoreKey Key = State->Sweep[State->SweepCursor++];
      const LodSection *Found = LodStoreFind(Store, Key.Level, Key.X, Key.Y, Key.Z);
      if (Found == NULL || !Found->Unsaved || !BusyAdd(State, Key)) {
        continue;
      }
      if (!LodSectionCopy(&Target->Sections[Target->Count], Found)) {
        BusyRelease(State, Key.Level, Key.X, Key.Z);
        State->SaveFailures++;
        continue;
      }
      LodStoreMarkSaved(Store, Key.Level, Key.X, Key.Y, Key.Z);
      AddItem(Target, Key);
    }
  }
  if (Job != NULL && Job->Count > 0) {
    atomic_store_explicit(&Job->Phase, SAVE_READY, memory_order_relaxed);
  }
}

// --- lifecycle --------------------------------------------------------------

static void FreePersistState(PersistState *State) {
  if (State->Jobs != NULL) {
    for (int Idx = 0; Idx < SAVE_JOBS; Idx++) {
      for (int Item = 0; Item < State->Jobs[Idx].Count; Item++) {
        LodSectionFree(&State->Jobs[Idx].Sections[Item]);
      }
    }
  }
  for (int Idx = 0; Idx < State->HeldCount; Idx++) {
    LodSectionFree(&State->Held[Idx].Section);
  }
  free(State->InFlight.Keys);
  LodCacheClose(State->Cache);
  free(State->Jobs);
  free(State->Held);
  free(State->Sweep);
  free(State->Busy);
  *State = (PersistState){0};
}

bool LodPersistInit(const char *Directory, uint64_t Seed) {
  PersistState *State = GetPersistState();
  *State = (PersistState){0};
  LodCacheConfig Config = {.Directory = Directory,
                           .Seed = Seed,
                           .WorldgenVersion = (uint32_t)WORLDGEN_VERSION,
                           .FormatVersion = LOD_CACHE_FORMAT_VERSION,
                           .Writable = true};
  State->Cache = LodCacheOpen(&Config);
  State->Jobs = (SaveJob *)calloc(SAVE_JOBS, sizeof(SaveJob));
  if (State->Cache == NULL || State->Jobs == NULL) {
    FreePersistState(State);
    LogWarn("LOD: disk cache off");
    return false;
  }
  for (int Idx = 0; Idx < SAVE_JOBS; Idx++) {
    atomic_init(&State->Jobs[Idx].Phase, SAVE_FREE);
  }
  State->Enabled = true;
  State->NextSweep = OsGetTimeSeconds() + SWEEP_INTERVAL_SECONDS;
  LodIngestSetEvictHook(OnEvict, State);
  return true;
}

bool LodPersistEnabled(void) { return GetPersistState()->Enabled; }

int LodPersistReadColumn(int Level, int X, int Z, LodCacheEntry *Out,
                         int Capacity, bool *Generated) {
  PersistState *State = GetPersistState();
  *Generated = false;
  if (!State->Enabled) {
    return -1;
  }
  return LodCacheReadColumn(State->Cache, Level, X, Z, Out, Capacity, Generated);
}

void LodPersistCountColumn(bool Hit, int SectionsLoaded) {
  PersistState *State = GetPersistState();
  if (!State->Enabled) {
    return;
  }
  if (Hit) {
    State->Hits++;
  } else {
    State->Misses++;
  }
  State->SectionsLoaded += (uint64_t)SectionsLoaded;
}

void LodPersistUpdate(bool WorldBusy, bool FillIdle) {
  PersistState *State = GetPersistState();
  if (!State->Enabled) {
    return;
  }
  PROFILE_BEGIN(PersistStart);
  for (int Idx = 0; Idx < SAVE_JOBS; Idx++) {
    if (atomic_load_explicit(&State->Jobs[Idx].Phase, memory_order_acquire) ==
        SAVE_DONE) {
      ReapJob(State, &State->Jobs[Idx]);
    }
  }
  InFlightRegions(State, &State->InFlight);
  PackHeld(State, &State->InFlight);
  LodStore *Store = LodIngestMutableStore();
  double Now = OsGetTimeSeconds();
  if (Store != NULL && !WorldBusy && FillIdle) {
    if (State->SweepCursor >= State->SweepCount && Now >= State->NextSweep) {
      State->NextSweep = Now + SWEEP_INTERVAL_SECONDS;
      if (!CollectUnsaved(State, Store)) {
        State->SweepCount = 0;
      }
    }
    CopySwept(State, Store, Now + COPY_BUDGET_SECONDS, &State->InFlight);
  }
  DispatchReady(State);
  PROFILE_END(PersistStart, PROFILE_LOD_INGEST);
}

bool LodPersistIdle(void) {
  const PersistState *State = GetPersistState();
  if (!State->Enabled) {
    return true;
  }
  if (State->HeldCount > 0 || State->SweepCursor < State->SweepCount) {
    return false;
  }
  for (int Idx = 0; Idx < SAVE_JOBS; Idx++) {
    if (atomic_load_explicit(&State->Jobs[Idx].Phase, memory_order_relaxed) !=
        SAVE_FREE) {
      return false;
    }
  }
  const LodStore *Store = LodIngestStore();
  for (int Idx = 0; Store != NULL && Idx < Store->Capacity; Idx++) {
    if (Store->Slots[Idx].Level > 0 && Store->Slots[Idx].Section->Unsaved) {
      return false;
    }
  }
  return true;
}

// Writes every held section (oldest copies first) on the calling thread,
// through the save jobs, which no worker runs any more.
static void SaveHeld(PersistState *State) {
  RegionSet None = {0};
  while (State->HeldCount > 0) {
    int Before = State->HeldCount;
    PackHeld(State, &None);
    for (int Idx = 0; Idx < SAVE_JOBS; Idx++) {
      if (atomic_load_explicit(&State->Jobs[Idx].Phase, memory_order_relaxed) ==
          SAVE_READY) {
        LodPersistRunJob(Idx);
        ReapJob(State, &State->Jobs[Idx]);
      }
    }
    None.Count = 0;
    if (State->HeldCount == Before) {
      break;
    }
  }
  free(None.Keys);
}

// Writes everything still unsaved in the store (newer than anything held), on
// the calling thread. Returns the sections written.
static int SaveRemaining(PersistState *State) {
  SaveHeld(State);
  LodIngestFlushAll();
  const LodStore *Store = LodIngestStore();
  if (Store == NULL || !CollectUnsaved(State, Store) || State->SweepCount == 0) {
    return 0;
  }
  LodCacheItem *Items =
      (LodCacheItem *)malloc(sizeof(LodCacheItem) * (size_t)State->SweepCount);
  if (Items == NULL) {
    State->SaveFailures += (uint64_t)State->SweepCount;
    return 0;
  }
  int Count = 0;
  for (int Idx = 0; Idx < State->SweepCount; Idx++) {
    LodStoreKey Key = State->Sweep[Idx];
    const LodSection *Found = LodStoreFind(Store, Key.Level, Key.X, Key.Y, Key.Z);
    if (Found != NULL) {
      Items[Count++] = (LodCacheItem){
          .Level = Key.Level, .X = Key.X, .Y = Key.Y, .Z = Key.Z, .Section = Found};
    }
  }
  int Written = Count;
  if (!LodCacheWrite(State->Cache, Items, Count)) {
    State->SaveFailures += (uint64_t)Count;
    Written = 0;
  }
  State->SweepCount = 0;
  State->SweepCursor = 0;
  free(Items);
  return Written;
}

void LodPersistShutdown(void) {
  PersistState *State = GetPersistState();
  if (!State->Enabled) {
    return;
  }
  double Start = OsGetTimeSeconds();
  LodIngestSetEvictHook(NULL, NULL);
  LodCacheStats Before;
  LodCacheGetStats(State->Cache, &Before);
  // The workers are gone: jobs they never ran are written here.
  for (int Idx = 0; Idx < SAVE_JOBS; Idx++) {
    int Phase = atomic_load_explicit(&State->Jobs[Idx].Phase, memory_order_acquire);
    if (Phase == SAVE_READY || Phase == SAVE_QUEUED) {
      LodPersistRunJob(Idx);
    }
    if (atomic_load_explicit(&State->Jobs[Idx].Phase, memory_order_acquire) ==
        SAVE_DONE) {
      ReapJob(State, &State->Jobs[Idx]);
    }
  }
  int Written = SaveRemaining(State);
  LodCacheStats After;
  LodCacheGetStats(State->Cache, &After);
  uint64_t Sections = After.SectionsWritten - Before.SectionsWritten;
  uint64_t Records = After.RecordBytesWritten - Before.RecordBytesWritten;
  uint64_t Memory = After.MemoryBytesWritten - Before.MemoryBytesWritten;
  LogInfo("LOD: disk cache saved %llu sections at shutdown (%d from the store) in "
          "%.1f ms, %.2f MB of records for %.2f MB in memory; %d regions, %.2f MB "
          "on disk, %llu hits, %llu misses, %llu not saved",
          (unsigned long long)Sections, Written,
          (OsGetTimeSeconds() - Start) * (double)MS_PER_SECOND,
          (double)Records / BYTES_PER_MEGABYTE, (double)Memory / BYTES_PER_MEGABYTE,
          After.Regions, (double)After.DiskBytes / BYTES_PER_MEGABYTE,
          (unsigned long long)State->Hits, (unsigned long long)State->Misses,
          (unsigned long long)State->SaveFailures);
  FreePersistState(State);
}

void LodPersistGetStats(LodPersistStats *Out) {
  PersistState *State = GetPersistState();
  *Out = (LodPersistStats){0};
  Out->Enabled = State->Enabled;
  if (!State->Enabled) {
    return;
  }
  Out->Hits = State->Hits;
  Out->Misses = State->Misses;
  Out->SectionsLoaded = State->SectionsLoaded;
  Out->SectionsQueued = State->SectionsQueued;
  Out->SaveFailures = State->SaveFailures;
  int Pending = State->HeldCount +
                (State->SweepCount - State->SweepCursor);
  for (int Idx = 0; Idx < SAVE_JOBS; Idx++) {
    if (atomic_load_explicit(&State->Jobs[Idx].Phase, memory_order_relaxed) !=
        SAVE_FREE) {
      Pending += State->Jobs[Idx].Count;
    }
  }
  Out->PendingSections = Pending;
  LodCacheGetStats(State->Cache, &Out->Cache);
}
