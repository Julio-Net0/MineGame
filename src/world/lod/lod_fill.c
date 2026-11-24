#include "world/lod/lod_fill.h"

#include "core/log.h"
#include "core/profiler.h"
#include "persistence/world_save.h"
#include "platform/os_time.h"
#include "world/chunk_worker.h"
#include "world/lod/lod_gen.h"
#include "world/lod/lod_ingest.h"
#include "world/lod/lod_persist.h"
#include "world/lod/lod_quality.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
  MAX_JOBS = 32,
  // The camera is taken at its chunk's centre, as the LOD renderer takes it.
  HALF_CHUNK = LOD_SECTION_SIZE / 2,
  // Jobs in flight while 1:1 chunks still load or mesh.
  BUSY_JOBS = 2,
  // Levels filled first, coarsest first: the horizon.
  COARSE_PHASE_LEVEL = 4,
  SET_INITIAL = 1024,
  CANDIDATES_INITIAL = 1024,
  LEVEL_KEY_SHIFT = 60,
  COORD_KEY_BITS = 30,
  MICROS_PER_SECOND = 1000000,
  MS_PER_SECOND = 1000
};

#define COORD_KEY_MASK ((1ULL << COORD_KEY_BITS) - 1ULL)
#define SET_HASH_MUL 0x9E3779B97F4A7C15ULL
// A parent section is twice as wide as its children.
#define PARENT_WIDTHS 2.0F
// Columns looked at beyond where a level can be wanted, in its own widths.
#define REACH_MARGIN_WIDTHS 3.0F
#define SET_EMPTY 0ULL
// The job cost the in-flight limits are set for (a generated column); once
// the 1:1 world is idle, cheaper jobs (columns read from the disk cache) may
// be that many times more numerous, so the worker time queued stays the same. The average starts
// there and follows the jobs that complete.
#define REFERENCE_JOB_MICROS 3500.0
#define JOB_COST_SMOOTHING 0.05
#define FLUSH_BUDGET_BUSY_SECONDS 0.0005
#define FLUSH_BUDGET_IDLE_SECONDS 0.002

typedef enum { JOB_FREE = 0, JOB_QUEUED, JOB_DONE } JobPhase;

typedef struct {
  atomic_int Phase;
  int Level;
  int X;
  int Z;
  uint64_t Seed;
  int Count;
  int Written;
  bool Failed;
  // A store write failed: the column is not whole in the store.
  bool WriteFailed;
  uint64_t Micros;
  // Worker time of the whole job: the cache read and any generation.
  uint64_t WorkMicros;
  // Sections read from the disk cache, written before the generated ones;
  // with CacheHit the column was cached whole and nothing was generated.
  int CachedCount;
  bool CacheHit;
  LodCacheEntry Cached[LOD_CACHE_MAX_COLUMN_SECTIONS];
  int SectionY[LOD_GEN_MAX_SECTIONS];
  uint64_t Present[LOD_GEN_MAX_SECTIONS];
  LodSection Sections[LOD_GEN_MAX_SECTIONS];
  LodGenScratch *Gen;
  LodEncodeScratch *Encode;
} FillJob;

typedef struct {
  int Level;
  int X;
  int Z;
  // Sort key: phase, then level or distance (see CompareCandidates).
  float Gap;
} Candidate;

typedef struct {
  bool Enabled;
  int DistanceChunks;
  // The renderer refines a section while the camera is within this many of
  // its widths (lod_quality.h); the fill wants the same columns.
  int RefineWidths;
  // The distance or the refine factor changed: candidates are rebuilt at the
  // next update.
  bool ViewChanged;
  int WorkerCount;
  double AverageJobMicros;
  bool HasAnchor;
  int AnchorX;
  int AnchorZ;
  int RenderDistance;

  // Columns dispatched or done for the current anchor, keyed by PackKey;
  // open addressing, SET_EMPTY marks a free slot.
  uint64_t *Set;
  int SetCapacity;
  int SetCount;

  Candidate *Candidates;
  int CandidateCount;
  int CandidateCapacity;
  int Cursor;
  int CoarseCount;

  FillJob *Jobs;

  double FillStart;
  double HorizonMs;
  double FillMs;
  bool WasIdle;
  LodFillStats Stats;
} LodFillState;

static LodFillState *GetFillState(void) {
  static LodFillState State;
  return &State;
}

// --- column set -------------------------------------------------------------

// Never SET_EMPTY: the level occupies the top bits and is at least 1.
static uint64_t PackKey(int Level, int X, int Z) {
  return ((uint64_t)Level << LEVEL_KEY_SHIFT) |
         (((uint64_t)(uint32_t)X & COORD_KEY_MASK) << COORD_KEY_BITS) |
         ((uint64_t)(uint32_t)Z & COORD_KEY_MASK);
}

static int UnpackCoord(uint64_t Bits) {
  uint32_t Value = (uint32_t)(Bits & COORD_KEY_MASK);
  // Sign-extend the 30-bit field.
  if ((Value & (1U << (COORD_KEY_BITS - 1))) != 0U) {
    Value |= ~(uint32_t)COORD_KEY_MASK;
  }
  return (int)Value;
}

static unsigned SetSlot(uint64_t Key, int Capacity) {
  uint64_t Mixed = Key * SET_HASH_MUL;
  return (unsigned)(Mixed >> COORD_KEY_BITS) & ((unsigned)Capacity - 1U);
}

static bool SetContains(const LodFillState *State, uint64_t Key) {
  unsigned Mask = (unsigned)State->SetCapacity - 1U;
  for (unsigned Idx = SetSlot(Key, State->SetCapacity);; Idx = (Idx + 1U) & Mask) {
    if (State->Set[Idx] == SET_EMPTY) {
      return false;
    }
    if (State->Set[Idx] == Key) {
      return true;
    }
  }
}

static void SetPlace(uint64_t *Set, int Capacity, uint64_t Key) {
  unsigned Mask = (unsigned)Capacity - 1U;
  unsigned Idx = SetSlot(Key, Capacity);
  while (Set[Idx] != SET_EMPTY) {
    Idx = (Idx + 1U) & Mask;
  }
  Set[Idx] = Key;
}

// Rebuilds the table at Capacity, keeping the keys Keep accepts (all of them
// when Keep is NULL). False when out of memory; the table is then unchanged.
static bool SetRebuild(LodFillState *State, int Capacity,
                       bool (*Keep)(uint64_t Key)) {
  uint64_t *Fresh = (uint64_t *)calloc((size_t)Capacity, sizeof(uint64_t));
  if (Fresh == NULL) {
    return false;
  }
  int Count = 0;
  for (int Idx = 0; Idx < State->SetCapacity; Idx++) {
    uint64_t Key = State->Set[Idx];
    if (Key != SET_EMPTY && (Keep == NULL || Keep(Key))) {
      SetPlace(Fresh, Capacity, Key);
      Count++;
    }
  }
  free(State->Set);
  State->Set = Fresh;
  State->SetCapacity = Capacity;
  State->SetCount = Count;
  return true;
}

static bool SetInsert(LodFillState *State, uint64_t Key) {
  if (SetContains(State, Key)) {
    return true;
  }
  if ((State->SetCount + 1) * 2 > State->SetCapacity &&
      !SetRebuild(State, State->SetCapacity * 2, NULL)) {
    return false;
  }
  SetPlace(State->Set, State->SetCapacity, Key);
  State->SetCount++;
  return true;
}

// A column the clipmap evicted must be generated again if it comes back.
static bool KeyInRange(uint64_t Key) {
  int Level = (int)(Key >> LEVEL_KEY_SHIFT);
  return LodIngestColumnInRange(Level, UnpackCoord(Key >> COORD_KEY_BITS),
                                UnpackCoord(Key));
}

// --- jobs -------------------------------------------------------------------

static void EmitSection(void *User, int SectionY,
                        const LodVoxel Voxels[LOD_SECTION_VOLUME],
                        uint64_t PresentCells) {
  FillJob *Job = (FillJob *)User;
  if (Job->Count >= LOD_GEN_MAX_SECTIONS) {
    return;
  }
  LodSection *Section = &Job->Sections[Job->Count];
  LodSectionInit(Section);
  if (!LodSectionEncodeWithScratch(Section, Voxels, Job->Encode)) {
    Job->Failed = true;
    return;
  }
  Job->SectionY[Job->Count] = SectionY;
  Job->Present[Job->Count] = PresentCells;
  Job->Count++;
}

static void RunFillJob(int JobId) {
  if (JobId >= LOD_PERSIST_JOB_BASE) {
    LodPersistRunJob(JobId - LOD_PERSIST_JOB_BASE);
    return;
  }
  LodFillState *State = GetFillState();
  if (JobId < 0 || JobId >= MAX_JOBS || State->Jobs == NULL) {
    return;
  }
  FillJob *Job = &State->Jobs[JobId];
  double JobStart = OsGetTimeSeconds();
  bool Generated = false;
  int Read = LodPersistReadColumn(Job->Level, Job->X, Job->Z, Job->Cached,
                                  LOD_CACHE_MAX_COLUMN_SECTIONS, &Generated);
  Job->CachedCount = (Read > 0) ? Read : 0;
  Job->CacheHit = Generated && Read > 0;
  if (!Job->CacheHit) {
    PROFILE_BEGIN(GenStart);
    double Start = OsGetTimeSeconds();
    LodGenResult Result;
    LodGenerateColumn(Job->Gen, Job->Level, Job->X, Job->Z, Job->Seed,
                      EmitSection, Job, &Result);
    Job->Micros =
        (uint64_t)((OsGetTimeSeconds() - Start) * (double)MICROS_PER_SECOND);
    PROFILE_END(GenStart, PROFILE_LOD_GEN);
  }
  Job->WorkMicros =
      (uint64_t)((OsGetTimeSeconds() - JobStart) * (double)MICROS_PER_SECOND);
  atomic_store_explicit(&Job->Phase, JOB_DONE, memory_order_release);
}

static void FreeJobSections(FillJob *Job) {
  for (int Idx = 0; Idx < Job->Count; Idx++) {
    LodSectionFree(&Job->Sections[Idx]);
  }
  for (int Idx = 0; Idx < Job->CachedCount; Idx++) {
    LodSectionFree(&Job->Cached[Idx].Section);
  }
  Job->Count = 0;
  Job->CachedCount = 0;
  Job->Written = 0;
}

static void FreeFillState(LodFillState *State) {
  if (State->Jobs != NULL) {
    for (int Idx = 0; Idx < MAX_JOBS; Idx++) {
      FreeJobSections(&State->Jobs[Idx]);
      LodGenScratchFree(State->Jobs[Idx].Gen);
      LodEncodeScratchFree(State->Jobs[Idx].Encode);
    }
  }
  free(State->Jobs);
  free(State->Set);
  free(State->Candidates);
  *State = (LodFillState){0};
}

bool LodFillInit(int DistanceChunks, int WorkerCount) {
  LodFillState *State = GetFillState();
  *State = (LodFillState){0};
  if (!LodIngestEnabled()) {
    return false;
  }
  State->Jobs = (FillJob *)calloc((size_t)MAX_JOBS, sizeof(FillJob));
  State->Set = (uint64_t *)calloc((size_t)SET_INITIAL, sizeof(uint64_t));
  State->Candidates =
      (Candidate *)malloc((size_t)CANDIDATES_INITIAL * sizeof(Candidate));
  bool Ready = State->Jobs != NULL && State->Set != NULL && State->Candidates != NULL;
  for (int Idx = 0; Ready && Idx < MAX_JOBS; Idx++) {
    FillJob *Job = &State->Jobs[Idx];
    atomic_init(&Job->Phase, JOB_FREE);
    Job->Gen = LodGenScratchCreate();
    Job->Encode = LodEncodeScratchCreate();
    Ready = Job->Gen != NULL && Job->Encode != NULL;
  }
  if (!Ready) {
    FreeFillState(State);
    LogWarn("LOD: could not allocate the synthetic fill; it stays off");
    return false;
  }
  State->SetCapacity = SET_INITIAL;
  State->CandidateCapacity = CANDIDATES_INITIAL;
  State->DistanceChunks = DistanceChunks;
  State->RefineWidths = LodQualityRefineFactor(LOD_QUALITY_MEDIUM);
  State->WorkerCount = (WorkerCount > 0) ? WorkerCount : 1;
  State->AverageJobMicros = REFERENCE_JOB_MICROS;
  State->FillStart = -1.0;
  State->HorizonMs = -1.0;
  State->FillMs = -1.0;
  State->Enabled = true;
  SetChunkWorkerLodRunner(RunFillJob);
  LogInfo("LOD: synthetic fill to %d chunks", DistanceChunks);
  return true;
}

void LodFillShutdown(void) {
  SetChunkWorkerLodRunner((LodJobRunner)0);
  FreeFillState(GetFillState());
}

bool LodFillEnabled(void) { return GetFillState()->Enabled; }

void LodFillSetView(int DistanceChunks, int RefineWidths) {
  LodFillState *State = GetFillState();
  if (!State->Enabled ||
      (DistanceChunks == State->DistanceChunks && RefineWidths == State->RefineWidths)) {
    return;
  }
  State->DistanceChunks = DistanceChunks;
  State->RefineWidths = RefineWidths;
  State->ViewChanged = true;
  // A new view is a new fill: its timings start over even if one is running.
  State->FillStart = -1.0;
}

// --- column choice ----------------------------------------------------------

static float AxisGap(float Value, float Min, float Max) {
  if (Value < Min) {
    return Min - Value;
  }
  return (Value > Max) ? Value - Max : 0.0F;
}

static float BoxGap(float CamX, float CamZ, int Level, int X, int Z) {
  float Width = (float)(LOD_SECTION_SIZE << Level);
  float MinX = (float)X * Width;
  float MinZ = (float)Z * Width;
  float Dx = AxisGap(CamX, MinX, MinX + Width);
  float Dz = AxisGap(CamZ, MinZ, MinZ + Width);
  return sqrtf((Dx * Dx) + (Dz * Dz));
}

static bool AppendCandidate(LodFillState *State, Candidate Entry) {
  if (State->CandidateCount == State->CandidateCapacity) {
    int Capacity = (State->CandidateCapacity > 0) ? State->CandidateCapacity * 2
                                                  : CANDIDATES_INITIAL;
    Candidate *Grown = (Candidate *)realloc(
        State->Candidates, (size_t)Capacity * sizeof(Candidate));
    if (Grown == NULL) {
      return false;
    }
    State->Candidates = Grown;
    State->CandidateCapacity = Capacity;
  }
  State->Candidates[State->CandidateCount++] = Entry;
  return true;
}

// Coarse levels first, coarsest first and nearest first within a level; then
// the fine levels nearest first, the coarser of two at one distance first.
// Positions break the remaining ties, so the order is fixed.
static int CompareCandidates(const void *Left, const void *Right) {
  const Candidate *A = (const Candidate *)Left;
  const Candidate *B = (const Candidate *)Right;
  bool CoarseA = A->Level >= COARSE_PHASE_LEVEL;
  bool CoarseB = B->Level >= COARSE_PHASE_LEVEL;
  if (CoarseA != CoarseB) {
    return CoarseA ? -1 : 1;
  }
  if (CoarseA && A->Level != B->Level) {
    return (A->Level > B->Level) ? -1 : 1;
  }
  if (A->Gap != B->Gap) {
    return (A->Gap < B->Gap) ? -1 : 1;
  }
  int Keys[2][3] = {{-A->Level, A->X, A->Z}, {-B->Level, B->X, B->Z}};
  for (int Key = 0; Key < 3; Key++) {
    if (Keys[0][Key] != Keys[1][Key]) {
      return (Keys[0][Key] < Keys[1][Key]) ? -1 : 1;
    }
  }
  return 0;
}

// Whether the renderer can want the level-Level column: within the distance
// and (below the top level) under a parent the camera is close enough to
// refine or that touches the 1:1 disc. Columns inside the disc are generated
// too: the 1:1 world loads a vertical window, not whole columns, and the
// renderer already hides the LOD wherever chunks are drawn, so real data
// only takes over the cells it actually holds.
static bool ColumnWanted(float CamX, float CamZ, int Level, int X, int Z,
                         float Distance, float Disc, int RefineWidths) {
  float Width = (float)(LOD_SECTION_SIZE << Level);
  if (BoxGap(CamX, CamZ, Level, X, Z) >= Distance) {
    return false;
  }
  if (Level == LOD_MAX_LEVEL) {
    return true;
  }
  float ParentGap = BoxGap(CamX, CamZ, Level + 1, LodFloorShift(X, 1),
                           LodFloorShift(Z, 1));
  float Refine = (float)RefineWidths * PARENT_WIDTHS * Width;
  return ParentGap < Refine + Width || ParentGap < Disc + Width;
}

static void BuildCandidates(LodFillState *State) {
  State->CandidateCount = 0;
  State->Cursor = 0;
  float Half = (float)HALF_CHUNK;
  float CamX = ((float)State->AnchorX * (float)LOD_SECTION_SIZE) + Half;
  float CamZ = ((float)State->AnchorZ * (float)LOD_SECTION_SIZE) + Half;
  float Distance = (float)State->DistanceChunks * (float)LOD_SECTION_SIZE;
  float Disc = (float)State->RenderDistance * (float)LOD_SECTION_SIZE;
  for (int Level = LOD_MAX_LEVEL; Level >= 1; Level--) {
    float Width = (float)(LOD_SECTION_SIZE << Level);
    float Reach = Distance;
    if (Level < LOD_MAX_LEVEL) {
      float Margin = REACH_MARGIN_WIDTHS * Width;
      float Refine = ((float)State->RefineWidths * PARENT_WIDTHS * Width) + Margin;
      Reach = fminf(Distance, fmaxf(Refine, Disc + Margin));
    }
    int Radius = (int)(Reach / Width) + 1;
    int CentreX = LodFloorShift(State->AnchorX, Level);
    int CentreZ = LodFloorShift(State->AnchorZ, Level);
    for (int X = CentreX - Radius; X <= CentreX + Radius; X++) {
      for (int Z = CentreZ - Radius; Z <= CentreZ + Radius; Z++) {
        if (SetContains(State, PackKey(Level, X, Z)) ||
            !LodIngestColumnInRange(Level, X, Z) ||
            !ColumnWanted(CamX, CamZ, Level, X, Z, Distance, Disc,
                          State->RefineWidths)) {
          continue;
        }
        Candidate Entry = {.Level = Level,
                           .X = X,
                           .Z = Z,
                           .Gap = BoxGap(CamX, CamZ, Level, X, Z)};
        if (!AppendCandidate(State, Entry)) {
          LogWarn("LOD: synthetic fill candidate list out of memory");
          return;
        }
      }
    }
  }
  qsort(State->Candidates, (size_t)State->CandidateCount, sizeof(Candidate),
        CompareCandidates);
  State->CoarseCount = 0;
  while (State->CoarseCount < State->CandidateCount &&
         State->Candidates[State->CoarseCount].Level >= COARSE_PHASE_LEVEL) {
    State->CoarseCount++;
  }
}

static void Reanchor(LodFillState *State, int ChunkX, int ChunkZ,
                     int RenderDistance) {
  if (State->WasIdle || State->FillStart < 0.0) {
    State->FillStart = OsGetTimeSeconds();
    State->HorizonMs = -1.0;
    State->FillMs = -1.0;
  }
  State->HasAnchor = true;
  State->AnchorX = ChunkX;
  State->AnchorZ = ChunkZ;
  State->RenderDistance = RenderDistance;
  int Capacity = State->SetCapacity;
  while (Capacity > SET_INITIAL && State->SetCount * 4 < Capacity) {
    Capacity /= 2;
  }
  (void)SetRebuild(State, Capacity, KeyInRange);
  BuildCandidates(State);
}

// --- per frame --------------------------------------------------------------

// Writes a finished job's sections while the budget lasts; true once the job
// is fully written and its slot free again.
// One section of a finished job: the cached ones first, then the generated.
static LodWriteResult WriteJobSection(FillJob *Job, int Idx) {
  if (Idx < Job->CachedCount) {
    LodCacheEntry *Entry = &Job->Cached[Idx];
    return LodIngestWriteCached(Job->Level, Job->X, Entry->Y, Job->Z,
                                &Entry->Section, Entry->Section.PresentCells,
                                Entry->Section.IngestedCells, Job->CacheHit);
  }
  int Gen = Idx - Job->CachedCount;
  return LodIngestWriteSynthetic(Job->Level, Job->X, Job->SectionY[Gen], Job->Z,
                                 &Job->Sections[Gen], Job->Present[Gen]);
}

static bool WriteJob(LodFillState *State, FillJob *Job, double Deadline) {
  int Total = Job->CachedCount + Job->Count;
  while (Job->Written < Total) {
    int Idx = Job->Written++;
    LodWriteResult Result = WriteJobSection(Job, Idx);
    if (Result == LOD_WRITE_APPLIED) {
      State->Stats.SectionsWritten++;
    } else if (Result == LOD_WRITE_SKIPPED_PRECEDENCE) {
      State->Stats.SectionsSkipped++;
    } else {
      State->Stats.SectionsFailed++;
      Job->WriteFailed = true;
    }
    if (Job->Written < Total && OsGetTimeSeconds() >= Deadline) {
      return false;
    }
  }
  if (Job->CacheHit) {
    State->Stats.ColumnsLoaded++;
    State->Stats.SectionsLoaded += (uint64_t)Job->CachedCount;
  } else {
    State->Stats.ColumnsGenerated++;
    State->Stats.SectionsGenerated += (uint64_t)Job->Count;
    State->Stats.GenMicros += Job->Micros;
    // The whole column is in the store now: its cached copy will not need
    // generating again.
    for (int Idx = 0; !Job->Failed && !Job->WriteFailed && Idx < Job->Count; Idx++) {
      LodIngestMarkGenerated(Job->Level, Job->X, Job->SectionY[Idx], Job->Z);
    }
  }
  LodPersistCountColumn(Job->CacheHit, Job->CachedCount);
  State->AverageJobMicros +=
      ((double)Job->WorkMicros - State->AverageJobMicros) * JOB_COST_SMOOTHING;
  if (Job->Failed) {
    State->Stats.SectionsFailed++;
  }
  FreeJobSections(Job);
  atomic_store_explicit(&Job->Phase, JOB_FREE, memory_order_relaxed);
  return true;
}

static int DrainJobs(LodFillState *State, double Deadline) {
  int Busy = 0;
  for (int Idx = 0; Idx < MAX_JOBS; Idx++) {
    FillJob *Job = &State->Jobs[Idx];
    int Phase = atomic_load_explicit(&Job->Phase, memory_order_acquire);
    if (Phase == JOB_DONE && OsGetTimeSeconds() < Deadline) {
      if (WriteJob(State, Job, Deadline)) {
        Phase = JOB_FREE;
      }
    }
    if (Phase != JOB_FREE) {
      Busy++;
    }
  }
  return Busy;
}

static void Dispatch(LodFillState *State, int Busy, bool WorldBusy) {
  // One worker is always left to the 1:1 world, so a chunk that starts
  // loading never waits for a whole round of LOD jobs.
  int Base = WorldBusy ? BUSY_JOBS : State->WorkerCount - 1;
  if (Base < 1) {
    Base = 1;
  }
  // Cheap jobs only widen the limit once the 1:1 world is idle: while it
  // loads, even short reads on every worker delay its chunks.
  double Scale = REFERENCE_JOB_MICROS / fmax(State->AverageJobMicros, 1.0);
  int Limit = (!WorldBusy && Scale > 1.0)
                  ? (int)((double)Base * fmin(Scale, (double)MAX_JOBS))
                  : Base;
  if (Limit > MAX_JOBS) {
    Limit = MAX_JOBS;
  }
  uint64_t Seed = GetWorldSeed();
  for (int Idx = 0; Idx < MAX_JOBS && Busy < Limit; Idx++) {
    FillJob *Job = &State->Jobs[Idx];
    if (atomic_load_explicit(&Job->Phase, memory_order_relaxed) != JOB_FREE) {
      continue;
    }
    while (State->Cursor < State->CandidateCount &&
           SetContains(State, PackKey(State->Candidates[State->Cursor].Level,
                                      State->Candidates[State->Cursor].X,
                                      State->Candidates[State->Cursor].Z))) {
      State->Cursor++;
    }
    if (State->Cursor >= State->CandidateCount) {
      return;
    }
    const Candidate *Next = &State->Candidates[State->Cursor];
    // A save of this column's region is still on its way to the disk: reading
    // it now could see an older copy.
    if (LodPersistColumnBusy(Next->Level, Next->X, Next->Z) ||
        !SetInsert(State, PackKey(Next->Level, Next->X, Next->Z))) {
      return;
    }
    Job->Level = Next->Level;
    Job->X = Next->X;
    Job->Z = Next->Z;
    Job->Seed = Seed;
    Job->Count = 0;
    Job->CachedCount = 0;
    Job->CacheHit = false;
    Job->Micros = 0;
    Job->Written = 0;
    Job->Failed = false;
    Job->WriteFailed = false;
    atomic_store_explicit(&Job->Phase, JOB_QUEUED, memory_order_release);
    if (!EnqueueChunkLodJob(Idx)) {
      atomic_store_explicit(&Job->Phase, JOB_FREE, memory_order_relaxed);
      return;
    }
    State->Cursor++;
    Busy++;
  }
}

void LodFillUpdate(int ChunkX, int ChunkZ, int RenderDistance, bool WorldBusy) {
  LodFillState *State = GetFillState();
  if (!State->Enabled) {
    return;
  }
  PROFILE_BEGIN(FillStart);
  if (!State->HasAnchor || State->ViewChanged || ChunkX != State->AnchorX ||
      ChunkZ != State->AnchorZ || RenderDistance != State->RenderDistance) {
    State->ViewChanged = false;
    Reanchor(State, ChunkX, ChunkZ, RenderDistance);
  }
  double Deadline = OsGetTimeSeconds() + (WorldBusy ? FLUSH_BUDGET_BUSY_SECONDS
                                                    : FLUSH_BUDGET_IDLE_SECONDS);
  int Busy = DrainJobs(State, Deadline);
  Dispatch(State, Busy, WorldBusy);

  int InFlight = 0;
  for (int Idx = 0; Idx < MAX_JOBS; Idx++) {
    if (atomic_load_explicit(&State->Jobs[Idx].Phase, memory_order_relaxed) !=
        JOB_FREE) {
      InFlight++;
    }
  }
  State->Stats.InFlight = InFlight;
  State->Stats.Queued = State->CandidateCount - State->Cursor;
  double Elapsed = (OsGetTimeSeconds() - State->FillStart) * (double)MS_PER_SECOND;
  if (State->HorizonMs < 0.0 && State->Cursor >= State->CoarseCount) {
    bool CoarseInFlight = false;
    for (int Idx = 0; Idx < MAX_JOBS; Idx++) {
      const FillJob *Job = &State->Jobs[Idx];
      CoarseInFlight |=
          atomic_load_explicit(&Job->Phase, memory_order_relaxed) != JOB_FREE &&
          Job->Level >= COARSE_PHASE_LEVEL;
    }
    if (!CoarseInFlight) {
      State->HorizonMs = Elapsed;
    }
  }
  State->WasIdle = State->Stats.Queued == 0 && InFlight == 0;
  if (State->WasIdle && State->FillMs < 0.0) {
    State->FillMs = Elapsed;
    LogInfo("LOD: synthetic fill done in %.0f ms (horizon %.0f ms), %llu "
            "columns generated, %llu loaded from the cache, %llu sections",
            State->FillMs, State->HorizonMs,
            (unsigned long long)State->Stats.ColumnsGenerated,
            (unsigned long long)State->Stats.ColumnsLoaded,
            (unsigned long long)State->Stats.SectionsWritten);
  }
  PROFILE_END(FillStart, PROFILE_LOD_INGEST);
}

bool LodFillIdle(void) {
  const LodFillState *State = GetFillState();
  return !State->Enabled || (State->HasAnchor && State->WasIdle);
}

void LodFillGetStats(LodFillStats *Out) {
  const LodFillState *State = GetFillState();
  *Out = State->Stats;
  Out->Enabled = State->Enabled;
  Out->DistanceChunks = State->DistanceChunks;
  Out->HorizonMs = State->HorizonMs;
  Out->FillMs = State->FillMs;
}
