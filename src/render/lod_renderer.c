#include "render/lod_renderer.h"

#include "core/log.h"
#include "core/profiler.h"
#include "platform/os_time.h"
#include "render/voxel_mesher.h"
#include "world/lod/lod_ingest.h"
#include "world/lod/lod_store.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
  COVERAGE_SPAN = RENDER_LOD_COVERAGE_SPAN,
  COVERAGE_MASK = RENDER_LOD_COVERAGE_SPAN - 1,
  COVERAGE_WORDS = RENDER_LOD_COVERAGE_WORDS,
  COVERAGE_BIAS = RENDER_LOD_COVERAGE_LAYER_BIAS,
  COVERAGE_LAYERS = RENDER_LOD_COVERAGE_LAYERS,
  WORD_BITS = 32,
  // Coverage is consulted only this close to the camera, in chunks: the GPU
  // side does the same, which keeps the toroidal grid unambiguous.
  COVERAGE_RANGE = 255,
  // Covered columns are also counted per cell of CELL_COLUMNS^2 columns, so a
  // far section's footprint is resolved per cell instead of per column.
  CELL_SHIFT = 4,
  CELL_COLUMNS = 1 << CELL_SHIFT,
  CELL_FULL = CELL_COLUMNS * CELL_COLUMNS,
  CELL_SPAN = RENDER_LOD_COVERAGE_SPAN >> CELL_SHIFT,
  CELL_MASK = CELL_SPAN - 1,
  CACHE_INITIAL = 4096,
  MAX_DRAWS = 32768,
  // Sections meshed per frame at most, on top of the time budget.
  MESH_BUDGET_SECTIONS = 96,
  // Cached meshes not drawn for this many frames, or whose section is gone,
  // are freed; the cache is swept every EVICT_INTERVAL frames.
  EVICT_INTERVAL = 64,
  EVICT_AGE = 600,
  // A section is refined while the camera is closer than this many of its
  // own widths: about two screen pixels per voxel at 1280x720 and 90 degrees.
  REFINE_FACTOR = 8,
  OCTANTS = 8,
  CELLS_PER_OCTANT_AXIS = 2,
  SLOT_EMPTY = 0,
  SLOT_TOMBSTONE = -1,
  LOAD_NUMERATOR = 3,
  LOAD_DENOMINATOR = 4,
  // A section and its six face neighbours.
  NEIGHBOURHOOD = 7,
  // Runs kept free for a section's translucent faces: one per direction.
  TRANSLUCENT_RUN_RESERVE = 6,
  // The packed vertex's face normal (see ChunkVertex).
  VERTEX_NORMAL_SHIFT = 27,
  VERTEX_NORMAL_MASK = 7,
  HALF_SECTION = LOD_SECTION_SIZE / 2
};

#define MESH_BUDGET_SECONDS 0.002
#define CULL_STATS_INTERVAL 0.5
// Blocks are centred on integers, so a box of blocks starts half a block low.
#define BLOCK_CENTRE_OFFSET 0.5F
#define CACHE_HASH_X 0x9E3779B97F4A7C15ULL
#define CACHE_HASH_Y 0xC2B2AE3D27D4EB4FULL
#define CACHE_HASH_Z 0x165667B19E3779F9ULL
#define CACHE_HASH_LEVEL 0x27D4EB2F165667C5ULL
#define CACHE_HASH_SHIFT 29U

typedef enum {
  COVERAGE_NONE = 0,
  COVERAGE_PARTIAL,
  COVERAGE_FULL
} SectionCoverage;

typedef struct {
  int32_t X;
  int32_t Y;
  int32_t Z;
  // 1..LOD_MAX_LEVEL when used, SLOT_EMPTY or SLOT_TOMBSTONE otherwise.
  int32_t Level;
  uint32_t Revision;
  RenderLodMesh Mesh;
  int VertexCount;
  uint32_t LastFrame;
  // A neighbour changed since the mesh was built: its border faces may be off.
  bool Stale;
} CacheEntry;

typedef struct {
  bool Enabled;
  bool DrawOn;
  uint32_t *Coverage;
  uint16_t *CellCounts;
  uint8_t *ColumnDirty;
  int *DirtyColumns;
  int DirtyCount;

  CacheEntry *Cache;
  int Capacity;
  int Count;
  int Tombstones;

  VoxelMesher *Mesher;
  VoxelLodScratch *Scratch;
  ChunkVertex *Upload;
  RenderLodDraw *Draws;
  RenderLodDraw *BorderDraws;

  // Counts selection passes, not frames: a pass reruns only when its inputs
  // changed (see LodRendererDraw), and mesh ages are measured in passes.
  uint32_t Frame;
  uint64_t JournalRead;
  bool HasSelection;
  int SelectChunkX;
  int SelectChunkY;
  int SelectChunkZ;
  // Bumped by every selection pass, so the backend re-sends the list only
  // when it changed.
  uint32_t ListRevision;
  int OpaqueCount;
  int BorderCount;
  LodRenderStats Stats;
} LodRendererState;

typedef struct {
  const LodStore *Store;
  Vec3 Camera;
  int CameraChunkX;
  int CameraChunkZ;
  double Deadline;
  int Meshed;
  int OpaqueCount;
  int BorderCount;
  int Waiting;
  int Starved;
  int Selected;
  bool ArenaFull;
} SelectContext;

static LodRendererState *GetLodRendererState(void) {
  static LodRendererState State;
  return &State;
}

bool LodRendererEnabled(void) { return GetLodRendererState()->Enabled; }

static void FreeRendererState(LodRendererState *State) {
  free(State->Coverage);
  free(State->CellCounts);
  free(State->ColumnDirty);
  free(State->DirtyColumns);
  free(State->Cache);
  free(State->Mesher);
  free(State->Scratch);
  free(State->Upload);
  free(State->Draws);
  free(State->BorderDraws);
  *State = (LodRendererState){0};
}

void LodRendererInit(void) {
  LodRendererState *State = GetLodRendererState();
  *State = (LodRendererState){0};
  if (!RenderLodActive() || !LodIngestEnabled()) {
    return;
  }
  size_t Columns = (size_t)COVERAGE_SPAN * COVERAGE_SPAN;
  State->Coverage = (uint32_t *)calloc(Columns * COVERAGE_WORDS, sizeof(uint32_t));
  State->CellCounts = (uint16_t *)calloc((size_t)CELL_SPAN * CELL_SPAN, sizeof(uint16_t));
  State->ColumnDirty = (uint8_t *)calloc(Columns, sizeof(uint8_t));
  State->DirtyColumns = (int *)malloc(Columns * sizeof(int));
  State->Cache = (CacheEntry *)calloc(CACHE_INITIAL, sizeof(CacheEntry));
  State->Mesher = (VoxelMesher *)malloc(sizeof(VoxelMesher));
  State->Scratch = (VoxelLodScratch *)malloc(sizeof(VoxelLodScratch));
  State->Upload = (ChunkVertex *)malloc(sizeof(ChunkVertex) * CHUNK_MAX_VERTICES);
  State->Draws = (RenderLodDraw *)malloc(sizeof(RenderLodDraw) * MAX_DRAWS);
  State->BorderDraws = (RenderLodDraw *)malloc(sizeof(RenderLodDraw) * MAX_DRAWS);
  if (State->Coverage == NULL || State->CellCounts == NULL ||
      State->ColumnDirty == NULL ||
      State->DirtyColumns == NULL || State->Cache == NULL ||
      State->Mesher == NULL || State->Scratch == NULL || State->Upload == NULL ||
      State->Draws == NULL || State->BorderDraws == NULL) {
    FreeRendererState(State);
    LogWarn("LOD: could not allocate the LOD renderer; LOD stays undrawn");
    return;
  }
  State->Capacity = CACHE_INITIAL;
  State->Enabled = true;
  State->DrawOn = true;
}

void LodRendererSetDrawOn(bool On) {
  LodRendererState *State = GetLodRendererState();
  if (!State->Enabled || State->DrawOn == On) {
    return;
  }
  State->DrawOn = On;
  State->HasSelection = false;
  State->Stats = (LodRenderStats){0};
}

bool LodRendererDrawOn(void) {
  const LodRendererState *State = GetLodRendererState();
  return State->Enabled && State->DrawOn;
}

void LodRendererShutdown(void) {
  LodRendererState *State = GetLodRendererState();
  if (State->Enabled) {
    for (int Idx = 0; Idx < State->Capacity; Idx++) {
      if (State->Cache[Idx].Level > 0) {
        RenderLodFreeMesh(State->Cache[Idx].Mesh);
      }
    }
  }
  FreeRendererState(State);
}

// --- coverage ------------------------------------------------------------------

static int ColumnIndex(int ChunkX, int ChunkZ) {
  return ((ChunkZ & COVERAGE_MASK) * COVERAGE_SPAN) + (ChunkX & COVERAGE_MASK);
}

static int CellIndex(int ChunkX, int ChunkZ) {
  return (((ChunkZ >> CELL_SHIFT) & CELL_MASK) * CELL_SPAN) +
         ((ChunkX >> CELL_SHIFT) & CELL_MASK);
}

static bool ColumnCovered(const LodRendererState *State, int Column) {
  const uint32_t *Words = &State->Coverage[(size_t)Column * COVERAGE_WORDS];
  return (Words[0] | Words[1] | Words[2] | Words[3]) != 0U;
}

void LodRendererSetChunkCovered(int ChunkX, int ChunkY, int ChunkZ, bool Covered) {
  LodRendererState *State = GetLodRendererState();
  int Layer = ChunkY + COVERAGE_BIAS;
  if (!State->Enabled || Layer < 0 || Layer >= COVERAGE_LAYERS) {
    return;
  }
  int Column = ColumnIndex(ChunkX, ChunkZ);
  uint32_t *Word = &State->Coverage[((size_t)Column * COVERAGE_WORDS) +
                                    ((size_t)Layer / WORD_BITS)];
  uint32_t Bit = 1U << ((unsigned)Layer % WORD_BITS);
  uint32_t Updated = Covered ? (*Word | Bit) : (*Word & ~Bit);
  if (Updated == *Word) {
    return;
  }
  bool Before = ColumnCovered(State, Column);
  *Word = Updated;
  bool After = ColumnCovered(State, Column);
  if (Before != After) {
    uint16_t *Cell = &State->CellCounts[CellIndex(ChunkX, ChunkZ)];
    *Cell = After ? (uint16_t)(*Cell + 1U) : (uint16_t)(*Cell - 1U);
  }
  if (State->ColumnDirty[Column] == 0U) {
    State->ColumnDirty[Column] = 1U;
    State->DirtyColumns[State->DirtyCount++] = Column;
  }
}

// True when any column changed.
static bool FlushCoverage(LodRendererState *State) {
  bool Changed = State->DirtyCount > 0;
  for (int Idx = 0; Idx < State->DirtyCount; Idx++) {
    int Column = State->DirtyColumns[Idx];
    State->ColumnDirty[Column] = 0U;
    RenderLodSetCoverage(Column % COVERAGE_SPAN, Column / COVERAGE_SPAN,
                         &State->Coverage[(size_t)Column * COVERAGE_WORDS]);
  }
  State->DirtyCount = 0;
  return Changed;
}

// The 1:1 area is a set of chunk columns: a column with any 1:1 chunk belongs
// to it at every height, so the LOD is strictly the far field (see
// lod_border.frag). A section is covered as far as its footprint is.
static SectionCoverage CoverageOf(const LodRendererState *State,
                                  const SelectContext *Ctx, int Level, int X,
                                  int Z) {
  int Size = 1 << Level;
  int ChunkX0 = X * Size;
  int ChunkZ0 = Z * Size;
  bool Gap = false;
  int MinX = Ctx->CameraChunkX - COVERAGE_RANGE;
  int MaxX = Ctx->CameraChunkX + COVERAGE_RANGE;
  int MinZ = Ctx->CameraChunkZ - COVERAGE_RANGE;
  int MaxZ = Ctx->CameraChunkZ + COVERAGE_RANGE;
  int FromX = (ChunkX0 > MinX) ? ChunkX0 : MinX;
  int ToX = (ChunkX0 + Size - 1 < MaxX) ? ChunkX0 + Size - 1 : MaxX;
  int FromZ = (ChunkZ0 > MinZ) ? ChunkZ0 : MinZ;
  int ToZ = (ChunkZ0 + Size - 1 < MaxZ) ? ChunkZ0 + Size - 1 : MaxZ;
  if (FromX > ToX || FromZ > ToZ) {
    return COVERAGE_NONE;
  }
  Gap = FromX != ChunkX0 || ToX != ChunkX0 + Size - 1 || FromZ != ChunkZ0 ||
        ToZ != ChunkZ0 + Size - 1;

  // An empty or full cell settles its whole part of the footprint; only mixed
  // cells (the 1:1 area's rim) are read column by column.
  bool Any = false;
  for (int CellX = FromX; CellX <= ToX; CellX = (CellX | (CELL_COLUMNS - 1)) + 1) {
    int EndX = (CellX | (CELL_COLUMNS - 1)) < ToX ? (CellX | (CELL_COLUMNS - 1)) : ToX;
    for (int CellZ = FromZ; CellZ <= ToZ; CellZ = (CellZ | (CELL_COLUMNS - 1)) + 1) {
      int EndZ = (CellZ | (CELL_COLUMNS - 1)) < ToZ ? (CellZ | (CELL_COLUMNS - 1)) : ToZ;
      unsigned Count = State->CellCounts[CellIndex(CellX, CellZ)];
      if (Count == 0U) {
        Gap = true;
      } else if (Count == (unsigned)CELL_FULL) {
        Any = true;
      } else {
        for (int ChunkX = CellX; ChunkX <= EndX; ChunkX++) {
          for (int ChunkZ = CellZ; ChunkZ <= EndZ; ChunkZ++) {
            bool Column = ColumnCovered(State, ColumnIndex(ChunkX, ChunkZ));
            Any = Any || Column;
            Gap = Gap || !Column;
          }
        }
      }
      if (Any && Gap) {
        return COVERAGE_PARTIAL;
      }
    }
  }
  if (!Any) {
    return COVERAGE_NONE;
  }
  return Gap ? COVERAGE_PARTIAL : COVERAGE_FULL;
}

// --- mesh cache ------------------------------------------------------------------

static uint64_t CacheHash(int Level, int X, int Y, int Z) {
  uint64_t Hash = ((uint64_t)(uint32_t)X * CACHE_HASH_X) ^
                  ((uint64_t)(uint32_t)Y * CACHE_HASH_Y) ^
                  ((uint64_t)(uint32_t)Z * CACHE_HASH_Z) ^
                  ((uint64_t)(uint32_t)Level * CACHE_HASH_LEVEL);
  return Hash ^ (Hash >> CACHE_HASH_SHIFT);
}

static CacheEntry *CacheFind(LodRendererState *State, int Level, int X, int Y,
                             int Z) {
  unsigned Mask = (unsigned)State->Capacity - 1U;
  unsigned Idx = (unsigned)CacheHash(Level, X, Y, Z) & Mask;
  for (int Probe = 0; Probe < State->Capacity; Probe++) {
    CacheEntry *Entry = &State->Cache[Idx];
    if (Entry->Level == SLOT_EMPTY) {
      return NULL;
    }
    if (Entry->Level == Level && Entry->X == X && Entry->Y == Y && Entry->Z == Z) {
      return Entry;
    }
    Idx = (Idx + 1U) & Mask;
  }
  return NULL;
}

static bool CacheRehash(LodRendererState *State, int Capacity) {
  CacheEntry *Entries = (CacheEntry *)calloc((size_t)Capacity, sizeof(CacheEntry));
  if (Entries == NULL) {
    return false;
  }
  unsigned Mask = (unsigned)Capacity - 1U;
  for (int Idx = 0; Idx < State->Capacity; Idx++) {
    const CacheEntry *Entry = &State->Cache[Idx];
    if (Entry->Level <= 0) {
      continue;
    }
    unsigned Slot =
        (unsigned)CacheHash(Entry->Level, Entry->X, Entry->Y, Entry->Z) & Mask;
    while (Entries[Slot].Level > 0) {
      Slot = (Slot + 1U) & Mask;
    }
    Entries[Slot] = *Entry;
  }
  free(State->Cache);
  State->Cache = Entries;
  State->Capacity = Capacity;
  State->Tombstones = 0;
  return true;
}

static CacheEntry *CacheInsert(LodRendererState *State, int Level, int X, int Y,
                               int Z) {
  int Used = State->Count + State->Tombstones + 1;
  if (Used * LOAD_DENOMINATOR > State->Capacity * LOAD_NUMERATOR) {
    bool Grow = (State->Count + 1) * 2 > State->Capacity;
    if (!CacheRehash(State, Grow ? State->Capacity * 2 : State->Capacity)) {
      return NULL;
    }
  }
  unsigned Mask = (unsigned)State->Capacity - 1U;
  unsigned Idx = (unsigned)CacheHash(Level, X, Y, Z) & Mask;
  while (State->Cache[Idx].Level > 0) {
    Idx = (Idx + 1U) & Mask;
  }
  if (State->Cache[Idx].Level == SLOT_TOMBSTONE) {
    State->Tombstones--;
  }
  State->Cache[Idx] = (CacheEntry){.X = X, .Y = Y, .Z = Z, .Level = Level,
                                   .Mesh = RENDER_LOD_MESH_INVALID};
  State->Count++;
  return &State->Cache[Idx];
}

static void EvictStale(LodRendererState *State, const LodStore *Store,
                       uint32_t MaxAge) {
  for (int Idx = 0; Idx < State->Capacity; Idx++) {
    CacheEntry *Entry = &State->Cache[Idx];
    if (Entry->Level <= 0) {
      continue;
    }
    bool Gone = LodStoreFind(Store, Entry->Level, Entry->X, Entry->Y, Entry->Z) == NULL;
    if (Gone || State->Frame - Entry->LastFrame > MaxAge) {
      RenderLodFreeMesh(Entry->Mesh);
      Entry->Level = SLOT_TOMBSTONE;
      State->Count--;
      State->Tombstones++;
    }
  }
}

// A section's mesh reads its six same-level neighbours, so every change the
// store journals marks the section and its neighbours for a rebuild.
// True when the store changed since the last call.
static bool ConsumeJournal(LodRendererState *State, const LodStore *Store) {
  static const int OFFSETS[NEIGHBOURHOOD][3] = {{0, 0, 0},  {-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
                                    {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
  uint64_t Seq = Store->JournalSeq;
  bool Changed = Seq != State->JournalRead;
  if (Seq - State->JournalRead > (uint64_t)LOD_JOURNAL_SIZE) {
    for (int Idx = 0; Idx < State->Capacity; Idx++) {
      State->Cache[Idx].Stale = true;
    }
    State->JournalRead = Seq;
    return Changed;
  }
  for (; State->JournalRead < Seq; State->JournalRead++) {
    LodStoreKey Key = Store->Journal[State->JournalRead & (LOD_JOURNAL_SIZE - 1U)];
    for (int Near = 0; Near < NEIGHBOURHOOD; Near++) {
      CacheEntry *Entry = CacheFind(State, Key.Level, Key.X + OFFSETS[Near][0],
                                    Key.Y + OFFSETS[Near][1], Key.Z + OFFSETS[Near][2]);
      if (Entry != NULL) {
        Entry->Stale = true;
      }
    }
  }
  return Changed;
}

static int QuadGroup(const ChunkVertex *Vertex) {
  int Normal = (int)((Vertex->Word0 >> VERTEX_NORMAL_SHIFT) & VERTEX_NORMAL_MASK);
  return (Normal < RENDER_LOD_AXIS_GROUPS) ? Normal : RENDER_LOD_AXIS_GROUPS;
}

// Splits the quads, in the order the mesher emitted them, into runs of one
// face group (see RenderLodUploadMesh). Past the run limit the rest becomes
// one run of the never-culled group, which only costs culling, not pixels.
static int BuildFaceRuns(const ChunkVertex *Vertices, int Count, RenderLodRun *Runs,
                         int MaxRuns) {
  int RunCount = 0;
  for (int Vertex = 0; Vertex < Count; Vertex += CHUNK_VERTICES_PER_QUAD) {
    int Group = QuadGroup(&Vertices[Vertex]);
    if (RunCount > 0 && (Runs[RunCount - 1].Group == Group || RunCount == MaxRuns)) {
      if (Runs[RunCount - 1].Group != Group) {
        Runs[RunCount - 1].Group = RENDER_LOD_AXIS_GROUPS;
      }
      Runs[RunCount - 1].Quads++;
      continue;
    }
    Runs[RunCount++] = (RenderLodRun){.Group = Group, .Quads = 1};
  }
  return RunCount;
}

// Meshes the section (with its same-level neighbours as margin) and uploads
// it. Returns the vertex count, or -1 when the arena had no room.
static int MeshSection(LodRendererState *State, const LodStore *Store,
                       const LodSection *Section, int Level, int X, int Y, int Z,
                       RenderLodMesh *OutMesh) {
  static const int OFFSETS[VOXEL_LOD_NEIGHBOURS][3] = {
      {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
  const LodSection *Neighbours[VOXEL_LOD_NEIGHBOURS];
  for (int Face = 0; Face < VOXEL_LOD_NEIGHBOURS; Face++) {
    Neighbours[Face] = LodStoreFind(Store, Level, X + OFFSETS[Face][0],
                                    Y + OFFSETS[Face][1], Z + OFFSETS[Face][2]);
  }
  PROFILE_BEGIN(MeshStart);
  VoxelLodPlace Place = {.Level = Level, .X = X, .Y = Y, .Z = Z};
  VoxelMeshLodSection(State->Mesher, State->Scratch, Section, &Place, Neighbours);
  // One mesh: the opaque (and cutout) faces, then the translucent ones,
  // which the LOD draws blended after every opaque face.
  int Count = 0;
  int OpaqueCount = 0;
  for (int Target = 0; Target < MESH_TARGET_COUNT; Target++) {
    const MeshScratch *Set = &State->Mesher->Sets[Target];
    int Take = Set->VCount;
    if (Count + Take > CHUNK_MAX_VERTICES) {
      Take = CHUNK_MAX_VERTICES - Count;
    }
    memcpy(&State->Upload[Count], Set->Vertices, (size_t)Take * sizeof(ChunkVertex));
    Count += Take;
    if (Target < MESH_TARGET_TRANSLUCENT) {
      OpaqueCount = Count;
    }
  }
  *OutMesh = RENDER_LOD_MESH_INVALID;
  int Result = Count;
  if (Count > 0) {
    RenderLodRun Runs[RENDER_LOD_MAX_RUNS];
    int Reserve = (Count > OpaqueCount) ? TRANSLUCENT_RUN_RESERVE : 0;
    int OpaqueRuns =
        BuildFaceRuns(State->Upload, OpaqueCount, Runs, RENDER_LOD_MAX_RUNS - Reserve);
    int RunCount = OpaqueRuns + BuildFaceRuns(&State->Upload[OpaqueCount],
                                              Count - OpaqueCount, &Runs[OpaqueRuns],
                                              RENDER_LOD_MAX_RUNS - OpaqueRuns);
    *OutMesh = RenderLodUploadMesh(State->Upload, Runs, RunCount, OpaqueRuns);
    if (*OutMesh == RENDER_LOD_MESH_INVALID) {
      Result = -1;
    }
  }
  PROFILE_END(MeshStart, PROFILE_LOD_MESH);
  return Result;
}

// The cached mesh for the section, rebuilt first when its content changed and
// the frame's budget allows; a stale mesh is drawn until then.
static const CacheEntry *EnsureMesh(LodRendererState *State, SelectContext *Ctx,
                                    const LodSection *Section, int Level, int X,
                                    int Y, int Z) {
  CacheEntry *Entry = CacheFind(State, Level, X, Y, Z);
  if (Entry != NULL && Entry->Revision == Section->Revision && !Entry->Stale &&
      (Entry->Mesh != RENDER_LOD_MESH_INVALID || Entry->VertexCount == 0)) {
    Entry->LastFrame = State->Frame;
    return Entry;
  }
  // A full arena is not waiting: the section only gets a mesh once eviction
  // frees room, so it must not hold back the settled state.
  if (Ctx->ArenaFull) {
    Ctx->Starved++;
    if (Entry != NULL) {
      Entry->LastFrame = State->Frame;
    }
    return Entry;
  }
  if (Ctx->Meshed >= MESH_BUDGET_SECTIONS || OsGetTimeSeconds() > Ctx->Deadline) {
    Ctx->Waiting++;
    if (Entry != NULL) {
      Entry->LastFrame = State->Frame;
    }
    return Entry;
  }
  RenderLodMesh Mesh = RENDER_LOD_MESH_INVALID;
  int Count = MeshSection(State, Ctx->Store, Section, Level, X, Y, Z, &Mesh);
  Ctx->Meshed++;
  if (Count < 0) {
    Ctx->ArenaFull = true;
    Ctx->Starved++;
    if (Entry != NULL) {
      Entry->LastFrame = State->Frame;
    }
    return Entry;
  }
  if (Entry == NULL) {
    Entry = CacheInsert(State, Level, X, Y, Z);
    if (Entry == NULL) {
      RenderLodFreeMesh(Mesh);
      return NULL;
    }
  } else {
    RenderLodFreeMesh(Entry->Mesh);
  }
  Entry->Mesh = Mesh;
  Entry->VertexCount = Count;
  Entry->Revision = Section->Revision;
  Entry->Stale = false;
  Entry->LastFrame = State->Frame;
  return Entry;
}

// --- selection -------------------------------------------------------------------

static uint64_t OctantCells(int Octant) {
  uint64_t Mask = 0;
  int Ox = (Octant & 1) * CELLS_PER_OCTANT_AXIS;
  int Oy = ((Octant >> 1) & 1) * CELLS_PER_OCTANT_AXIS;
  int Oz = ((Octant >> 2) & 1) * CELLS_PER_OCTANT_AXIS;
  for (int Cell = 0; Cell < OCTANTS; Cell++) {
    Mask |= LodCellBit(Ox + (Cell & 1), Oy + ((Cell >> 1) & 1),
                       Oz + ((Cell >> 2) & 1));
  }
  return Mask;
}

static bool ChildrenResident(const LodStore *Store, const LodSection *Section,
                             int Level, int X, int Y, int Z) {
  for (int Octant = 0; Octant < OCTANTS; Octant++) {
    if ((Section->PresentCells & OctantCells(Octant)) == 0U) {
      continue;
    }
    if (LodStoreFind(Store, Level - 1, (X * 2) + (Octant & 1),
                     (Y * 2) + ((Octant >> 1) & 1),
                     (Z * 2) + ((Octant >> 2) & 1)) == NULL) {
      return false;
    }
  }
  return true;
}

static float AxisGap(float Value, float Min, float Max) {
  if (Value < Min) {
    return Min - Value;
  }
  return (Value > Max) ? Value - Max : 0.0F;
}

static bool CameraIsNear(const SelectContext *Ctx, int Level, int X, int Y, int Z) {
  float Width = (float)(LOD_SECTION_SIZE << Level);
  float MinX = ((float)X * Width) - BLOCK_CENTRE_OFFSET;
  float MinY = ((float)Y * Width) - BLOCK_CENTRE_OFFSET;
  float MinZ = ((float)Z * Width) - BLOCK_CENTRE_OFFSET;
  float Dx = AxisGap(Ctx->Camera.x, MinX, MinX + Width);
  float Dy = AxisGap(Ctx->Camera.y, MinY, MinY + Width);
  float Dz = AxisGap(Ctx->Camera.z, MinZ, MinZ + Width);
  float Limit = Width * (float)REFINE_FACTOR;
  return (Dx * Dx) + (Dy * Dy) + (Dz * Dz) < Limit * Limit;
}

static void AddDraw(LodRendererState *State, SelectContext *Ctx,
                    const CacheEntry *Entry, int Level, int X, int Y, int Z,
                    bool Border) {
  if (Entry == NULL || Entry->Mesh == RENDER_LOD_MESH_INVALID ||
      Ctx->OpaqueCount + Ctx->BorderCount >= MAX_DRAWS) {
    return;
  }
  int Width = LOD_SECTION_SIZE << Level;
  RenderLodDraw Draw = {.Mesh = Entry->Mesh,
                        .OriginX = X * Width,
                        .OriginY = Y * Width,
                        .OriginZ = Z * Width,
                        .Scale = 1 << Level,
                        .VertexCount = Entry->VertexCount};
  if (Border) {
    State->BorderDraws[Ctx->BorderCount++] = Draw;
  } else {
    State->Draws[Ctx->OpaqueCount++] = Draw;
  }
}

static void SelectSection(LodRendererState *State, SelectContext *Ctx, int Level,
                          int X, int Y, int Z) {
  const LodSection *Section = LodStoreFind(Ctx->Store, Level, X, Y, Z);
  if (Section == NULL || Section->PresentCells == 0U) {
    return;
  }
  SectionCoverage Coverage = CoverageOf(State, Ctx, Level, X, Z);
  if (Coverage == COVERAGE_FULL) {
    return;
  }
  bool Refine = Coverage == COVERAGE_PARTIAL || CameraIsNear(Ctx, Level, X, Y, Z);
  if (Level > 1 && Refine &&
      ChildrenResident(Ctx->Store, Section, Level, X, Y, Z)) {
    for (int Octant = 0; Octant < OCTANTS; Octant++) {
      SelectSection(State, Ctx, Level - 1, (X * 2) + (Octant & 1),
                    (Y * 2) + ((Octant >> 1) & 1), (Z * 2) + ((Octant >> 2) & 1));
    }
    return;
  }
  Ctx->Selected++;
  const CacheEntry *Entry = EnsureMesh(State, Ctx, Section, Level, X, Y, Z);
  AddDraw(State, Ctx, Entry, Level, X, Y, Z, Coverage == COVERAGE_PARTIAL);
}

// The selection and its draw list depend only on the store, the coverage, the
// camera's chunk and the meshes still owed, so a frame where none of them
// changed resubmits the previous list. The camera is taken at its chunk's
// centre, which keeps the result independent of the path that led there.
void LodRendererDraw(RenderCamera Camera) {
  LodRendererState *State = GetLodRendererState();
  const LodStore *Store = LodIngestStore();
  if (!State->Enabled || !State->DrawOn || Store == NULL) {
    return;
  }
  PROFILE_BEGIN(DrawStart);
  bool CoverageChanged = FlushCoverage(State);
  bool StoreChanged = ConsumeJournal(State, Store);
  int ChunkX = (int)floorf(Camera.Position.x / (float)LOD_SECTION_SIZE);
  int ChunkY = (int)floorf(Camera.Position.y / (float)LOD_SECTION_SIZE);
  int ChunkZ = (int)floorf(Camera.Position.z / (float)LOD_SECTION_SIZE);
  bool Moved = ChunkX != State->SelectChunkX || ChunkY != State->SelectChunkY ||
               ChunkZ != State->SelectChunkZ;
  bool Owed = State->Stats.WaitingSections > 0 || State->Stats.StarvedSections > 0;
  if (!State->HasSelection || CoverageChanged || StoreChanged || Moved || Owed) {
    State->Frame++;
    State->ListRevision++;
    if (State->Frame % EVICT_INTERVAL == 0U) {
      EvictStale(State, Store, (uint32_t)EVICT_AGE);
    }
    float Half = (float)HALF_SECTION;
    SelectContext Ctx = {
        .Store = Store,
        .Camera = {((float)ChunkX * (float)LOD_SECTION_SIZE) + Half,
                   ((float)ChunkY * (float)LOD_SECTION_SIZE) + Half,
                   ((float)ChunkZ * (float)LOD_SECTION_SIZE) + Half},
        .CameraChunkX = ChunkX,
        .CameraChunkZ = ChunkZ,
        .Deadline = OsGetTimeSeconds() + MESH_BUDGET_SECONDS};
    for (int Idx = 0; Idx < Store->Capacity; Idx++) {
      const LodStoreSlot *Slot = &Store->Slots[Idx];
      if (Slot->Level == LOD_MAX_LEVEL) {
        SelectSection(State, &Ctx, LOD_MAX_LEVEL, Slot->X, Slot->Y, Slot->Z);
      }
    }
    memcpy(&State->Draws[Ctx.OpaqueCount], State->BorderDraws,
           (size_t)Ctx.BorderCount * sizeof(RenderLodDraw));
    State->HasSelection = true;
    State->SelectChunkX = ChunkX;
    State->SelectChunkY = ChunkY;
    State->SelectChunkZ = ChunkZ;
    State->OpaqueCount = Ctx.OpaqueCount;
    State->BorderCount = Ctx.BorderCount;
    State->Stats = (LodRenderStats){.Selected = Ctx.Selected,
                                    .OpaqueSections = Ctx.OpaqueCount,
                                    .BorderSections = Ctx.BorderCount,
                                    .MeshedSections = Ctx.Meshed,
                                    .WaitingSections = Ctx.Waiting,
                                    .StarvedSections = Ctx.Starved};
    if (Ctx.ArenaFull) {
      // Every mesh this pass did not select goes, so the next passes can mesh
      // the starved sections once the arena's delayed frees land.
      EvictStale(State, Store, 0U);
    }
  }
  RenderDrawLod(State->Draws, State->OpaqueCount, State->BorderCount,
                State->ListRevision);

  RenderGetLodStats(&State->Stats.Gpu);
  State->Stats.Enabled = true;
  State->Stats.CachedMeshes = State->Count;
  PROFILE_END(DrawStart, PROFILE_LOD_DRAW);
}

bool LodRendererIdle(void) {
  const LodRendererState *State = GetLodRendererState();
  return !State->Enabled || State->Stats.WaitingSections == 0;
}

void LodRendererReadCullStats(bool Force) {
  static double LastRead = -1.0;
  LodRendererState *State = GetLodRendererState();
  double Now = OsGetTimeSeconds();
  if (!State->Enabled || !State->DrawOn ||
      (!Force && LastRead >= 0.0 && Now - LastRead < CULL_STATS_INTERVAL)) {
    return;
  }
  LastRead = Now;
  RenderLodReadCullStats();
  RenderGetLodStats(&State->Stats.Gpu);
}

void LodRendererGetStats(LodRenderStats *Out) {
  const LodRendererState *State = GetLodRendererState();
  *Out = State->Stats;
  Out->Enabled = State->Enabled;
  Out->DrawOn = State->Enabled && State->DrawOn;
}
