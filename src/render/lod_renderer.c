#include "render/lod_renderer.h"

#include "core/log.h"
#include "core/profiler.h"
#include "platform/os_time.h"
#include "render/voxel_mesher.h"
#include "world/lod/lod_ingest.h"
#include "world/lod/lod_quality.h"
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
  // Per-column flags: the column counts as 1:1 (see RefreshColumn), and its top
  // terrain chunk left while chunks under it stay.
  COLUMN_READY = 1,
  COLUMN_STRIPPED = 2,
  CACHE_INITIAL = 4096,
  MAX_DRAWS = 32768,
  // Sections meshed per frame at most, on top of the time budget.
  MESH_BUDGET_SECTIONS = 96,
  // Eviction rounds allowed for one selection anchor (camera chunk, store and
  // coverage unchanged) while sections starve; past them the starved wait
  // for the anchor to change, so a full arena can never loop.
  MAX_EVICTION_ROUNDS = 4,
  // A round frees this many times the starved section's vertices, the slack
  // covering pages the freed meshes leave scattered.
  EVICTION_SLACK = 2,
  // Cached meshes not drawn for this many frames, or whose section is gone,
  // are freed; the cache is swept every EVICT_INTERVAL frames.
  EVICT_INTERVAL = 64,
  EVICT_AGE = 600,
  OCTANTS = 8,
  CELLS_PER_OCTANT_AXIS = 2,
  SLOT_EMPTY = 0,
  SLOT_TOMBSTONE = -1,
  LOAD_NUMERATOR = 3,
  LOAD_DENOMINATOR = 4,
  // Runs kept free for a section's translucent faces: one per direction.
  TRANSLUCENT_RUN_RESERVE = 6,
  // The packed vertex's face normal (see ChunkVertex).
  VERTEX_NORMAL_SHIFT = 27,
  VERTEX_NORMAL_MASK = 7,
  HALF_SECTION = LOD_SECTION_SIZE / 2,
  // Sections the selection walk reaches (selected or refined), kept to find
  // where neighbouring regions are drawn at different levels.
  WALK_SET_INITIAL = 65536,
  WALK_LEVEL_SHIFT = 61,
  WALK_Y_SHIFT = 52,
  WALK_X_SHIFT = 26,
  WALK_Y_BITS = 9,
  WALK_XZ_BITS = 26
};

typedef enum { WALK_NONE = 0, WALK_SELECTED, WALK_REFINED } WalkKind;

#define MESH_BUDGET_SECONDS 0.002
#define CULL_STATS_INTERVAL 0.5
// Blocks are centred on integers, so a box of blocks starts half a block low.
#define BLOCK_CENTRE_OFFSET 0.5F
// Margin subtracted from the 1:1 far clip plane when checking whether a covered
// section can still cross it: absorbs rounding the camera to its chunk centre.
#define NEAR_FAR_SELECT_MARGIN 32.0F
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
  // Squared distance from the camera's chunk centre at the last selection.
  float DistSq;
  // A neighbour changed since the mesh was built: its border faces may be off.
  bool Stale;
  // Faces the mesh treated as level transitions (see VoxelMeshLodSection).
  uint8_t Transitions;
  // The mesh carries skirts: only border draws use them, so only their
  // sections pay for them.
  bool Skirted;
  // Kept only to stand in for its children (see SelectSection), not drawn:
  // the first to go when the arena is full.
  bool Retained;
} CacheEntry;

// A selected section whose mesh is missing or out of date, queued so the
// nearest are meshed first.
typedef struct {
  const LodSection *Section;
  int32_t Level;
  int32_t X;
  int32_t Y;
  int32_t Z;
  float DistSq;
  // Position in the selection walk, which orders the draw list.
  uint32_t Key;
  bool Border;
  uint8_t Transitions;
  // Queued only to be meshed: a child its parent stands in for (see
  // ChildrenMeshed).
  bool Prefetch;
  // Drawn with the mesh it has, current or not, in place of its children.
  bool StandIn;
  // A refined section's kept mesh brought up to date with its content, with
  // whatever meshing time the frame has left, so it can stand in.
  bool Refresh;
} PendingMesh;

// A draw with its position in the selection walk.
typedef struct {
  uint32_t Key;
  RenderLodDraw Draw;
} KeyedDraw;

typedef struct {
  // One bit per chunk drawn 1:1 with terrain, and per chunk the world created
  // that has not resolved yet; see RefreshColumn.
  uint32_t *Coverage;
  uint32_t *Pending;
  uint8_t *ColumnFlags;
  uint16_t *CellCounts;
  uint8_t *ColumnDirty;
  int *DirtyColumns;
  CacheEntry *Cache;
  VoxelMesher *Mesher;
  VoxelLodScratch *Scratch;
  ChunkVertex *Upload;
  RenderLodDraw *Draws;
  KeyedDraw *OpaqueKeyed;
  KeyedDraw *BorderKeyed;
  PendingMesh *PendingMeshes;
  // The selection walk's sections, in walk order, and the set of every
  // section it selected or refined.
  PendingMesh *Walked;
  uint64_t *WalkKeys;
  uint8_t *WalkKinds;
  int WalkCapacity;
  int WalkCount;
  int64_t EvictedForRoom;
  uint64_t JournalRead;
  LodRenderStats Stats;

  int DirtyCount;
  int Capacity;
  int Count;
  int Tombstones;
  // The arena generation when sections last starved: they are retried only
  // once it moves (space freed or the arena grown), not every frame.
  uint32_t StarvedGeneration;
  int EvictionRounds;
  // Counts selection passes, not frames: a pass reruns only when its inputs
  // changed (see LodRendererDraw), and mesh ages are measured in passes.
  uint32_t Frame;
  int SelectChunkX;
  int SelectChunkY;
  int SelectChunkZ;
  // Bumped by every selection pass, so the backend re-sends the list only
  // when it changed.
  uint32_t ListRevision;
  int OpaqueCount;
  int BorderCount;

  bool Enabled;
  bool DrawOn;
  // A section is refined while the camera is closer than this many of its
  // own widths (lod_quality.h): medium's 8 is about two screen pixels per
  // voxel at 1280x720 and 90 degrees, low's 4 about four, high's 16 about one.
  int RefineFactor;
  // Out of eviction rounds for this anchor: starved sections wait for it to
  // change instead of retrying on every freed page.
  bool StarvedParked;
  bool HasSelection;
  // The last pass meshed children stood in for: the next pass refines them.
  bool PrefetchOwed;
  // Kept meshes still to refresh; they never hold back the idle state.
  bool RefreshOwed;
} LodRendererState;

typedef struct {
  const LodStore *Store;
  Vec3 Camera;
  float RefineFactor;
  int CameraChunkX;
  int CameraChunkZ;
  int Meshed;
  int OpaqueCount;
  int BorderCount;
  int Waiting;
  int Starved;
  int Selected;
  int PendingCount;
  int WalkedCount;
  // Sections selected so far: the walk position the next one takes.
  uint32_t WalkIndex;
  // Nearest starved section's squared distance, or negative when none.
  float NearestStarved;
  // Vertices of the first section the arena could not hold.
  int StarvedVertices;
  // Meshing attempts, the per-frame budget, and successful uploads.
  int Attempts;
  int PrefetchMeshed;
  int RefreshWaiting;
  bool ArenaFull;
} SelectContext;

static LodRendererState *GetLodRendererState(void) {
  static LodRendererState State;
  return &State;
}

bool LodRendererEnabled(void) { return GetLodRendererState()->Enabled; }

static void FreeRendererState(LodRendererState *State) {
  free(State->Coverage);
  free(State->Pending);
  free(State->ColumnFlags);
  free(State->CellCounts);
  free(State->ColumnDirty);
  free(State->DirtyColumns);
  free(State->Cache);
  free(State->Mesher);
  free(State->Scratch);
  free(State->Upload);
  free(State->Draws);
  free(State->OpaqueKeyed);
  free(State->BorderKeyed);
  free(State->PendingMeshes);
  free(State->Walked);
  free(State->WalkKeys);
  free(State->WalkKinds);
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
  State->Pending = (uint32_t *)calloc(Columns * COVERAGE_WORDS, sizeof(uint32_t));
  State->ColumnFlags = (uint8_t *)calloc(Columns, sizeof(uint8_t));
  State->CellCounts = (uint16_t *)calloc((size_t)CELL_SPAN * CELL_SPAN, sizeof(uint16_t));
  State->ColumnDirty = (uint8_t *)calloc(Columns, sizeof(uint8_t));
  State->DirtyColumns = (int *)malloc(Columns * sizeof(int));
  State->Cache = (CacheEntry *)calloc(CACHE_INITIAL, sizeof(CacheEntry));
  State->Mesher = (VoxelMesher *)malloc(sizeof(VoxelMesher));
  State->Scratch = (VoxelLodScratch *)malloc(sizeof(VoxelLodScratch));
  State->Upload = (ChunkVertex *)malloc(sizeof(ChunkVertex) * CHUNK_MAX_VERTICES);
  State->Draws = (RenderLodDraw *)malloc(sizeof(RenderLodDraw) * MAX_DRAWS);
  State->OpaqueKeyed = (KeyedDraw *)malloc(sizeof(KeyedDraw) * MAX_DRAWS);
  State->BorderKeyed = (KeyedDraw *)malloc(sizeof(KeyedDraw) * MAX_DRAWS);
  State->PendingMeshes = (PendingMesh *)malloc(sizeof(PendingMesh) * MAX_DRAWS);
  State->Walked = (PendingMesh *)malloc(sizeof(PendingMesh) * MAX_DRAWS);
  State->WalkKeys = (uint64_t *)calloc(WALK_SET_INITIAL, sizeof(uint64_t));
  State->WalkKinds = (uint8_t *)calloc(WALK_SET_INITIAL, sizeof(uint8_t));
  State->WalkCapacity = WALK_SET_INITIAL;
  if (State->Coverage == NULL || State->Pending == NULL ||
      State->ColumnFlags == NULL || State->CellCounts == NULL ||
      State->Walked == NULL || State->WalkKeys == NULL || State->WalkKinds == NULL ||
      State->ColumnDirty == NULL ||
      State->DirtyColumns == NULL || State->Cache == NULL ||
      State->Mesher == NULL || State->Scratch == NULL || State->Upload == NULL ||
      State->Draws == NULL || State->OpaqueKeyed == NULL || State->BorderKeyed == NULL ||
      State->PendingMeshes == NULL) {
    FreeRendererState(State);
    LogWarn("LOD: could not allocate the LOD renderer; LOD stays undrawn");
    return;
  }
  State->Capacity = CACHE_INITIAL;
  State->Enabled = true;
  State->RefineFactor = LodQualityRefineFactor(LOD_QUALITY_MEDIUM);
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

void LodRendererSetRefineFactor(int Factor) {
  LodRendererState *State = GetLodRendererState();
  if (!State->Enabled || Factor < 1 || State->RefineFactor == Factor) {
    return;
  }
  State->RefineFactor = Factor;
  // Meshes stay cached; only the walk is redone, at the next draw.
  State->HasSelection = false;
}

int LodRendererRefineFactor(void) { return GetLodRendererState()->RefineFactor; }

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
  return (State->ColumnFlags[Column] & COLUMN_READY) != 0U;
}

static uint32_t *ColumnWords(uint32_t *Bits, int Column) {
  return &Bits[(size_t)Column * COVERAGE_WORDS];
}

static bool AnyWord(const uint32_t *Words) {
  return (Words[0] | Words[1] | Words[2] | Words[3]) != 0U;
}

// Whether any bit above Layer is set.
static bool AnyAbove(const uint32_t *Words, int Layer) {
  int Word = Layer / WORD_BITS;
  unsigned Shift = ((unsigned)Layer % WORD_BITS) + 1U;
  uint32_t Higher = (Shift < (unsigned)WORD_BITS) ? (Words[Word] >> Shift) : 0U;
  for (int Next = Word + 1; Next < COVERAGE_WORDS; Next++) {
    Higher |= Words[Next];
  }
  return Higher != 0U;
}

static void SetBit(uint32_t *Words, int Layer, bool On) {
  uint32_t Bit = 1U << ((unsigned)Layer % WORD_BITS);
  if (On) {
    Words[Layer / WORD_BITS] |= Bit;
  } else {
    Words[Layer / WORD_BITS] &= ~Bit;
  }
}

static bool BitSet(const uint32_t *Words, int Layer) {
  return ((Words[Layer / WORD_BITS] >> ((unsigned)Layer % WORD_BITS)) & 1U) != 0U;
}

// A column hands its place over to the 1:1 chunks only once it is whole: it
// holds terrain and every chunk the world created in it has resolved. Cutting
// the LOD at the first resolved chunk (buried rock resolves at once, the
// surface waits for its neighbours and the workers) left the column hollow,
// sky showing through, for as long as the surface took. Once ready it stays
// so while chunks are added (a raised band, the vertical window moving), and
// it gives up only when its terrain empties or its top terrain chunk leaves:
// a column leaving the load volume loses its chunks over several frames, in
// any order, and the LOD has to be back before the surface is gone.
static void RefreshColumn(LodRendererState *State, int Column, int ChunkX,
                          int ChunkZ) {
  uint8_t Flags = State->ColumnFlags[Column];
  uint8_t Updated = Flags;
  if (!AnyWord(ColumnWords(State->Coverage, Column))) {
    Updated = 0U;
  } else if ((Updated & COLUMN_STRIPPED) != 0U) {
    Updated &= (uint8_t)~COLUMN_READY;
  } else if (!AnyWord(ColumnWords(State->Pending, Column))) {
    Updated |= COLUMN_READY;
  }
  State->ColumnFlags[Column] = Updated;
  bool Before = (Flags & COLUMN_READY) != 0U;
  bool After = (Updated & COLUMN_READY) != 0U;
  if (Before == After) {
    return;
  }
  uint16_t *Cell = &State->CellCounts[CellIndex(ChunkX, ChunkZ)];
  *Cell = After ? (uint16_t)(*Cell + 1U) : (uint16_t)(*Cell - 1U);
  if (State->ColumnDirty[Column] == 0U) {
    State->ColumnDirty[Column] = 1U;
    State->DirtyColumns[State->DirtyCount++] = Column;
  }
}

static int CoverageLayer(const LodRendererState *State, int ChunkY) {
  int Layer = ChunkY + COVERAGE_BIAS;
  return (State->Enabled && Layer >= 0 && Layer < COVERAGE_LAYERS) ? Layer : -1;
}

void LodRendererExpectChunk(int ChunkX, int ChunkY, int ChunkZ) {
  LodRendererState *State = GetLodRendererState();
  int Layer = CoverageLayer(State, ChunkY);
  if (Layer < 0) {
    return;
  }
  int Column = ColumnIndex(ChunkX, ChunkZ);
  SetBit(ColumnWords(State->Pending, Column), Layer, true);
  // Coming back into the volume: ready again once the new chunks resolve.
  State->ColumnFlags[Column] &= (uint8_t)~COLUMN_STRIPPED;
  RefreshColumn(State, Column, ChunkX, ChunkZ);
}

void LodRendererSetChunkCovered(int ChunkX, int ChunkY, int ChunkZ, bool Covered) {
  LodRendererState *State = GetLodRendererState();
  int Layer = CoverageLayer(State, ChunkY);
  if (Layer < 0) {
    return;
  }
  int Column = ColumnIndex(ChunkX, ChunkZ);
  SetBit(ColumnWords(State->Pending, Column), Layer, false);
  SetBit(ColumnWords(State->Coverage, Column), Layer, Covered);
  RefreshColumn(State, Column, ChunkX, ChunkZ);
}

void LodRendererForgetChunk(int ChunkX, int ChunkY, int ChunkZ) {
  LodRendererState *State = GetLodRendererState();
  int Layer = CoverageLayer(State, ChunkY);
  if (Layer < 0) {
    return;
  }
  int Column = ColumnIndex(ChunkX, ChunkZ);
  uint32_t *Coverage = ColumnWords(State->Coverage, Column);
  SetBit(ColumnWords(State->Pending, Column), Layer, false);
  if (BitSet(Coverage, Layer)) {
    SetBit(Coverage, Layer, false);
    // Rock under the terrain let go of (the window moving up) leaves the
    // surface above it drawn; the top terrain chunk leaving does not.
    if (!AnyAbove(Coverage, Layer)) {
      State->ColumnFlags[Column] |= COLUMN_STRIPPED;
    }
  }
  RefreshColumn(State, Column, ChunkX, ChunkZ);
}

// True when any column changed. The texture only tells covered columns from
// the others (lod_border.frag tests for any bit), so a column that is not
// ready yet goes up empty.
static bool FlushCoverage(LodRendererState *State) {
  static const uint32_t NONE[COVERAGE_WORDS] = {0U, 0U, 0U, 0U};
  bool Changed = State->DirtyCount > 0;
  for (int Idx = 0; Idx < State->DirtyCount; Idx++) {
    int Column = State->DirtyColumns[Idx];
    State->ColumnDirty[Column] = 0U;
    const uint32_t *Words =
        ColumnCovered(State, Column) ? ColumnWords(State->Coverage, Column) : NONE;
    RenderLodSetCoverage(Column % COVERAGE_SPAN, Column / COVERAGE_SPAN, Words);
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

static bool CoveredInRange(const LodRendererState *State, const SelectContext *Ctx,
                           int ChunkX, int ChunkZ) {
  int OffsetX = abs(ChunkX - Ctx->CameraChunkX);
  int OffsetZ = abs(ChunkZ - Ctx->CameraChunkZ);
  if (OffsetX > COVERAGE_RANGE || OffsetZ > COVERAGE_RANGE) {
    return false;
  }
  return ColumnCovered(State, ColumnIndex(ChunkX, ChunkZ));
}

// Whether a section with no 1:1 column of its own has one beside its sides.
// Its skirts on that side close the cut there (see BuildLodSkirtPass), and
// only the border pass draws skirts, so such a section is drawn by it too.
// Sections coarser than the last skirted level carry none.
static bool BesideCoverage(const LodRendererState *State, const SelectContext *Ctx,
                           int Level, int X, int Z) {
  if (Level > VOXEL_LOD_SKIRT_MAX_LEVEL) {
    return false;
  }
  int Size = 1 << Level;
  int ChunkX0 = X * Size;
  int ChunkZ0 = Z * Size;
  for (int Along = 0; Along < Size; Along++) {
    if (CoveredInRange(State, Ctx, ChunkX0 - 1, ChunkZ0 + Along) ||
        CoveredInRange(State, Ctx, ChunkX0 + Size, ChunkZ0 + Along) ||
        CoveredInRange(State, Ctx, ChunkX0 + Along, ChunkZ0 - 1) ||
        CoveredInRange(State, Ctx, ChunkX0 + Along, ChunkZ0 + Size)) {
      return true;
    }
  }
  return false;
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
                       uint32_t MaxAge, bool DropRetained) {
  for (int Idx = 0; Idx < State->Capacity; Idx++) {
    CacheEntry *Entry = &State->Cache[Idx];
    if (Entry->Level <= 0) {
      continue;
    }
    bool Gone = LodStoreFind(Store, Entry->Level, Entry->X, Entry->Y, Entry->Z) == NULL;
    if (Gone || State->Frame - Entry->LastFrame > MaxAge ||
        (DropRetained && Entry->Retained)) {
      RenderLodFreeMesh(Entry->Mesh);
      Entry->Level = SLOT_TOMBSTONE;
      State->Count--;
      State->Tombstones++;
    }
  }
}

// A section's mesh reads the 26 same-level sections around it (their faces,
// edges and corners fill its margin), so every change the store journals
// marks the section and those around it for a rebuild.
// True when the store changed since the last call.
static bool ConsumeJournal(LodRendererState *State, const LodStore *Store) {
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
    for (int Dx = -1; Dx <= 1; Dx++) {
      for (int Dy = -1; Dy <= 1; Dy++) {
        for (int Dz = -1; Dz <= 1; Dz++) {
          CacheEntry *Entry =
              CacheFind(State, Key.Level, Key.X + Dx, Key.Y + Dy, Key.Z + Dz);
          if (Entry != NULL) {
            Entry->Stale = true;
          }
        }
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
// it. Returns the vertex count; OutMesh stays invalid for a non-empty mesh
// the arena had no room for.
static int MeshSection(LodRendererState *State, const LodStore *Store,
                       const LodSection *Section, int Level, int X, int Y, int Z,
                       unsigned Transitions, bool Skirts, RenderLodMesh *OutMesh) {
  const LodSection *Around[VOXEL_LOD_AROUND];
  for (int Dx = -1; Dx <= 1; Dx++) {
    for (int Dy = -1; Dy <= 1; Dy++) {
      for (int Dz = -1; Dz <= 1; Dz++) {
        Around[VoxelLodAround(Dx, Dy, Dz)] =
            LodStoreFind(Store, Level, X + Dx, Y + Dy, Z + Dz);
      }
    }
  }
  PROFILE_BEGIN(MeshStart);
  VoxelLodPlace Place = {.Level = Level, .X = X, .Y = Y, .Z = Z, .Skirts = Skirts};
  VoxelMeshLodSection(State->Mesher, State->Scratch, Section, &Place, Around,
                      Transitions);
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
  if (Count > 0) {
    RenderLodRun Runs[RENDER_LOD_MAX_RUNS];
    int Reserve = (Count > OpaqueCount) ? TRANSLUCENT_RUN_RESERVE : 0;
    int OpaqueRuns =
        BuildFaceRuns(State->Upload, OpaqueCount, Runs, RENDER_LOD_MAX_RUNS - Reserve);
    int RunCount = OpaqueRuns + BuildFaceRuns(&State->Upload[OpaqueCount],
                                              Count - OpaqueCount, &Runs[OpaqueRuns],
                                              RENDER_LOD_MAX_RUNS - OpaqueRuns);
    *OutMesh = RenderLodUploadMesh(State->Upload, Runs, RunCount, OpaqueRuns);
  }
  PROFILE_END(MeshStart, PROFILE_LOD_MESH);
  return Count;
}

static bool MeshCurrent(const CacheEntry *Entry, const LodSection *Section,
                        unsigned Transitions, bool Skirts) {
  return Entry != NULL && Entry->Revision == Section->Revision && !Entry->Stale &&
         Entry->Transitions == Transitions && Entry->Skirted == Skirts &&
         (Entry->Mesh != RENDER_LOD_MESH_INVALID || Entry->VertexCount == 0);
}

// Meshes a queued section into its cache entry. False when the arena had no
// room; the entry (and any older mesh it holds) is then left as it was.
static bool MeshPending(LodRendererState *State, SelectContext *Ctx,
                        const PendingMesh *Pending, CacheEntry **InOutEntry) {
  RenderLodMesh Mesh = RENDER_LOD_MESH_INVALID;
  int Count = MeshSection(State, Ctx->Store, Pending->Section, Pending->Level,
                          Pending->X, Pending->Y, Pending->Z, Pending->Transitions,
                          Pending->Border, &Mesh);
  Ctx->Attempts++;
  if (Count > 0 && Mesh == RENDER_LOD_MESH_INVALID) {
    Ctx->StarvedVertices = Count;
    return false;
  }
  Ctx->Meshed++;
  CacheEntry *Entry = *InOutEntry;
  if (Entry == NULL) {
    Entry = CacheInsert(State, Pending->Level, Pending->X, Pending->Y, Pending->Z);
    if (Entry == NULL) {
      RenderLodFreeMesh(Mesh);
      return true;
    }
  } else {
    RenderLodFreeMesh(Entry->Mesh);
  }
  Entry->Mesh = Mesh;
  Entry->VertexCount = Count;
  Entry->Revision = Pending->Section->Revision;
  Entry->Stale = false;
  Entry->Transitions = Pending->Transitions;
  Entry->Skirted = Pending->Border;
  Entry->LastFrame = State->Frame;
  Entry->DistSq = Pending->DistSq;
  *InOutEntry = Entry;
  return true;
}

// --- walk set ----------------------------------------------------------------------

static uint64_t WalkKey(int Level, int X, int Y, int Z) {
  uint64_t HeightMask = (1ULL << WALK_Y_BITS) - 1ULL;
  uint64_t PlaneMask = (1ULL << WALK_XZ_BITS) - 1ULL;
  return ((uint64_t)Level << WALK_LEVEL_SHIFT) |
         (((uint64_t)(uint32_t)Y & HeightMask) << WALK_Y_SHIFT) |
         (((uint64_t)(uint32_t)X & PlaneMask) << WALK_X_SHIFT) |
         ((uint64_t)(uint32_t)Z & PlaneMask);
}

static unsigned WalkSlot(uint64_t Key, int Capacity) {
  return (unsigned)((Key * CACHE_HASH_X) >> CACHE_HASH_SHIFT) &
         ((unsigned)Capacity - 1U);
}

static void WalkReset(LodRendererState *State) {
  memset(State->WalkKinds, 0, (size_t)State->WalkCapacity);
  State->WalkCount = 0;
}

static WalkKind WalkFind(const LodRendererState *State, int Level, int X, int Y,
                         int Z) {
  uint64_t Key = WalkKey(Level, X, Y, Z);
  unsigned Mask = (unsigned)State->WalkCapacity - 1U;
  for (unsigned Idx = WalkSlot(Key, State->WalkCapacity);; Idx = (Idx + 1U) & Mask) {
    if (State->WalkKinds[Idx] == WALK_NONE) {
      return WALK_NONE;
    }
    if (State->WalkKeys[Idx] == Key) {
      return (WalkKind)State->WalkKinds[Idx];
    }
  }
}

static void WalkPlace(uint64_t *Keys, uint8_t *Kinds, int Capacity, uint64_t Key,
                      uint8_t Kind) {
  unsigned Mask = (unsigned)Capacity - 1U;
  unsigned Idx = WalkSlot(Key, Capacity);
  while (Kinds[Idx] != WALK_NONE) {
    Idx = (Idx + 1U) & Mask;
  }
  Keys[Idx] = Key;
  Kinds[Idx] = Kind;
}

// Records a walked section. When the set cannot grow, the section is left
// out: its neighbours then see no transition there, as before transitions.
static void WalkMark(LodRendererState *State, int Level, int X, int Y, int Z,
                     WalkKind Kind) {
  if ((State->WalkCount + 1) * 2 > State->WalkCapacity) {
    int Capacity = State->WalkCapacity * 2;
    uint64_t *Keys = (uint64_t *)malloc((size_t)Capacity * sizeof(uint64_t));
    uint8_t *Kinds = (uint8_t *)calloc((size_t)Capacity, sizeof(uint8_t));
    if (Keys == NULL || Kinds == NULL) {
      free(Keys);
      free(Kinds);
      return;
    }
    for (int Idx = 0; Idx < State->WalkCapacity; Idx++) {
      if (State->WalkKinds[Idx] != WALK_NONE) {
        WalkPlace(Keys, Kinds, Capacity, State->WalkKeys[Idx], State->WalkKinds[Idx]);
      }
    }
    free(State->WalkKeys);
    free(State->WalkKinds);
    State->WalkKeys = Keys;
    State->WalkKinds = Kinds;
    State->WalkCapacity = Capacity;
  }
  WalkPlace(State->WalkKeys, State->WalkKinds, State->WalkCapacity,
            WalkKey(Level, X, Y, Z), (uint8_t)Kind);
  State->WalkCount++;
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
  float Limit = Width * Ctx->RefineFactor;
  return (Dx * Dx) + (Dy * Dy) + (Dz * Dz) < Limit * Limit;
}

// Squared distance from the camera's chunk centre to the section's centre.
static float SectionDistSq(const SelectContext *Ctx, int Level, int X, int Y, int Z) {
  float Width = (float)(LOD_SECTION_SIZE << Level);
  float Half = (float)(LOD_SECTION_SIZE << (Level - 1));
  float Dx = ((float)X * Width) + Half - BLOCK_CENTRE_OFFSET - Ctx->Camera.x;
  float Dy = ((float)Y * Width) + Half - BLOCK_CENTRE_OFFSET - Ctx->Camera.y;
  float Dz = ((float)Z * Width) + Half - BLOCK_CENTRE_OFFSET - Ctx->Camera.z;
  return (Dx * Dx) + (Dy * Dy) + (Dz * Dz);
}

static void AddDraw(LodRendererState *State, SelectContext *Ctx,
                    const CacheEntry *Entry, int Level, int X, int Y, int Z,
                    bool Border, uint32_t Key) {
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
    State->BorderKeyed[Ctx->BorderCount++] = (KeyedDraw){.Key = Key, .Draw = Draw};
  } else {
    State->OpaqueKeyed[Ctx->OpaqueCount++] = (KeyedDraw){.Key = Key, .Draw = Draw};
  }
}

static int CompareKeyed(const void *Left, const void *Right) {
  uint32_t A = ((const KeyedDraw *)Left)->Key;
  uint32_t B = ((const KeyedDraw *)Right)->Key;
  return (A > B) - (A < B);
}

// The draw list in selection-walk order: sections meshed this pass were
// added after the walk, in distance order, and go back to their walk place,
// so the list (and blending and depth-tie order with it) never depends on
// which pass meshed what. Opaque draws first, then border draws.
static void AssembleDraws(LodRendererState *State, const SelectContext *Ctx) {
  if (Ctx->PendingCount > 0) {
    qsort(State->OpaqueKeyed, (size_t)Ctx->OpaqueCount, sizeof(KeyedDraw), CompareKeyed);
    qsort(State->BorderKeyed, (size_t)Ctx->BorderCount, sizeof(KeyedDraw), CompareKeyed);
  }
  for (int Idx = 0; Idx < Ctx->OpaqueCount; Idx++) {
    State->Draws[Idx] = State->OpaqueKeyed[Idx].Draw;
  }
  for (int Idx = 0; Idx < Ctx->BorderCount; Idx++) {
    State->Draws[Ctx->OpaqueCount + Idx] = State->BorderKeyed[Idx].Draw;
  }
}

// How a section's AABB sits relative to the 1:1 far clip sphere:
//   *OutAllPast: every corner lies past the 1:1 far plane (no 1:1 fragment can
//                overlap it, so it draws opaque at its natural LOD level);
//   *OutReaches: at least one corner reaches the 1:1 far plane, so a covered
//                section must still enter the border pass.
static void SectionNearFarRange(const SelectContext *Ctx, int Level, int X,
                                int Y, int Z, bool *OutAllPast,
                                bool *OutReaches) {
  float Width = (float)(LOD_SECTION_SIZE << Level);
  float MinX = ((float)X * Width) - BLOCK_CENTRE_OFFSET;
  float MinY = ((float)Y * Width) - BLOCK_CENTRE_OFFSET;
  float MinZ = ((float)Z * Width) - BLOCK_CENTRE_OFFSET;
  float NearX = AxisGap(Ctx->Camera.x, MinX, MinX + Width);
  float NearY = AxisGap(Ctx->Camera.y, MinY, MinY + Width);
  float NearZ = AxisGap(Ctx->Camera.z, MinZ, MinZ + Width);
  float FarX = fmaxf(fabsf(Ctx->Camera.x - MinX),
                     fabsf(Ctx->Camera.x - (MinX + Width)));
  float FarY = fmaxf(fabsf(Ctx->Camera.y - MinY),
                     fabsf(Ctx->Camera.y - (MinY + Width)));
  float FarZ = fmaxf(fabsf(Ctx->Camera.z - MinZ),
                     fabsf(Ctx->Camera.z - (MinZ + Width)));
  float FarPlane = RenderGetNearFarPlane();
  float Low = FarPlane - NEAR_FAR_SELECT_MARGIN;
  float High = FarPlane + NEAR_FAR_SELECT_MARGIN;
  float NearDistSq = (NearX * NearX) + (NearY * NearY) + (NearZ * NearZ);
  float FarDistSq = (FarX * FarX) + (FarY * FarY) + (FarZ * FarZ);
  *OutAllPast = NearDistSq >= High * High;
  *OutReaches = FarDistSq >= Low * Low;
}

typedef struct {
  SectionCoverage Coverage;
  // Wholly under 1:1 chunks, short of the 1:1 far plane: not drawn at all.
  bool Hidden;
  // Drawn by the border pass.
  bool Border;
} SectionRole;

static SectionRole RoleOf(const LodRendererState *State, const SelectContext *Ctx,
                          int Level, int X, int Y, int Z) {
  SectionCoverage Coverage = CoverageOf(State, Ctx, Level, X, Z);
  bool Beside = Coverage == COVERAGE_NONE && BesideCoverage(State, Ctx, Level, X, Z);
  bool FarStraddles = false;
  if (Coverage != COVERAGE_NONE) {
    bool AllPast = false;
    bool Reaches = false;
    SectionNearFarRange(Ctx, Level, X, Y, Z, &AllPast, &Reaches);
    if (AllPast) {
      Coverage = COVERAGE_NONE;
    } else if (Coverage == COVERAGE_FULL && Reaches) {
      FarStraddles = true;
    }
  }
  return (SectionRole){
      .Coverage = Coverage,
      .Hidden = Coverage == COVERAGE_FULL && !FarStraddles,
      .Border = Coverage == COVERAGE_PARTIAL || FarStraddles || Beside};
}

static void WalkAppend(LodRendererState *State, SelectContext *Ctx,
                       PendingMesh Walk) {
  if (Ctx->WalkedCount < MAX_DRAWS) {
    State->Walked[Ctx->WalkedCount++] = Walk;
  }
}

// Whether every child that would be drawn already has a mesh, old or not.
// Those without one are queued (not drawn) so a later pass can refine. A
// region refines the moment 1:1 coverage reaches it or the camera nears it;
// children without meshes then left it empty, sky showing through, for as
// many frames as meshing them took.
static bool ChildrenMeshed(LodRendererState *State, SelectContext *Ctx,
                           const LodSection *Section, int Level, int X, int Y,
                           int Z) {
  bool Meshed = true;
  for (int Octant = 0; Octant < OCTANTS; Octant++) {
    if ((Section->PresentCells & OctantCells(Octant)) == 0U) {
      continue;
    }
    int ChildX = (X * 2) + (Octant & 1);
    int ChildY = (Y * 2) + ((Octant >> 1) & 1);
    int ChildZ = (Z * 2) + ((Octant >> 2) & 1);
    const LodSection *Child = LodStoreFind(Ctx->Store, Level - 1, ChildX, ChildY, ChildZ);
    if (Child == NULL || Child->PresentCells == 0U ||
        CacheFind(State, Level - 1, ChildX, ChildY, ChildZ) != NULL) {
      continue;
    }
    SectionRole Role = RoleOf(State, Ctx, Level - 1, ChildX, ChildY, ChildZ);
    if (Role.Hidden) {
      continue;
    }
    Meshed = false;
    WalkAppend(State, Ctx,
               (PendingMesh){.Section = Child, .Level = Level - 1, .X = ChildX,
                             .Y = ChildY, .Z = ChildZ,
                             .DistSq = SectionDistSq(Ctx, Level - 1, ChildX, ChildY, ChildZ),
                             .Border = Role.Border, .Prefetch = true});
  }
  return Meshed;
}

static void SelectSection(LodRendererState *State, SelectContext *Ctx, int Level,
                          int X, int Y, int Z) {
  const LodSection *Section = LodStoreFind(Ctx->Store, Level, X, Y, Z);
  if (Section == NULL) {
    return;
  }
  SectionRole Role = RoleOf(State, Ctx, Level, X, Y, Z);
  if (Role.Hidden) {
    return;
  }
  bool Refine = Role.Coverage == COVERAGE_PARTIAL || CameraIsNear(Ctx, Level, X, Y, Z);
  bool StandIn = false;
  if (Level > 1 && Refine &&
      ChildrenResident(Ctx->Store, Section, Level, X, Y, Z)) {
    // A section with a mesh stands in for its children until they have
    // theirs, and keeps that mesh while refined for the next time. Only a
    // mesh of its current content: one kept from before the section filled
    // in would show holes of its own.
    CacheEntry *Own = CacheFind(State, Level, X, Y, Z);
    if (Own != NULL) {
      Own->LastFrame = State->Frame;
      Own->Retained = true;
    }
    bool OwnCurrent = Own != NULL && Own->Revision == Section->Revision;
    StandIn = OwnCurrent && Own->Mesh != RENDER_LOD_MESH_INVALID &&
              !ChildrenMeshed(State, Ctx, Section, Level, X, Y, Z);
    if (Own != NULL && !OwnCurrent && Section->PresentCells != 0U) {
      WalkAppend(State, Ctx,
                 (PendingMesh){.Section = Section, .Level = Level, .X = X, .Y = Y,
                               .Z = Z, .DistSq = SectionDistSq(Ctx, Level, X, Y, Z),
                               .Border = Role.Border, .Refresh = true});
    }
    if (!StandIn) {
      WalkMark(State, Level, X, Y, Z, WALK_REFINED);
      for (int Octant = 0; Octant < OCTANTS; Octant++) {
        SelectSection(State, Ctx, Level - 1, (X * 2) + (Octant & 1),
                      (Y * 2) + ((Octant >> 1) & 1), (Z * 2) + ((Octant >> 2) & 1));
      }
      return;
    }
  }
  // Nothing worth drawing at this level (buried rock), but it is still walked
  // above: a coarse synthetic section can read as buried where its finer
  // children, sampled more densely, hold a lake or a valley.
  if (Section->PresentCells == 0U) {
    return;
  }
  Ctx->Selected++;
  uint32_t Key = Ctx->WalkIndex++;
  WalkMark(State, Level, X, Y, Z, WALK_SELECTED);
  WalkAppend(State, Ctx,
             (PendingMesh){.Section = Section, .Level = Level, .X = X, .Y = Y,
                           .Z = Z, .DistSq = SectionDistSq(Ctx, Level, X, Y, Z),
                           .Key = Key, .Border = Role.Border, .StandIn = StandIn});
}

// Side faces of a selected section whose neighbour on its level is not drawn
// itself while its region is: refined into finer sections, or inside a
// coarser selected one. A neighbour no walk reached (under 1:1 chunks, or
// holding nothing to draw) is no transition. Only sides: the surface steps
// between levels show from above as cracks in a side, while a top or bottom
// face at a transition would lay a section-wide sheet of buried rock under
// the sea.
static unsigned TransitionFaces(const LodRendererState *State, const PendingMesh *Walk) {
  static const int SIDES[4][3] = {{-1, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1}};
  static const int SIDE_FACES[4] = {0, 1, 4, 5};
  unsigned Faces = 0;
  for (int Side = 0; Side < 4; Side++) {
    int Face = SIDE_FACES[Side];
    int X = Walk->X + SIDES[Side][0];
    int Y = Walk->Y + SIDES[Side][1];
    int Z = Walk->Z + SIDES[Side][2];
    WalkKind Kind = WalkFind(State, Walk->Level, X, Y, Z);
    bool Transition = Kind == WALK_REFINED;
    for (int Level = Walk->Level + 1; Kind == WALK_NONE && Level <= LOD_MAX_LEVEL;
         Level++) {
      int Shift = Level - Walk->Level;
      Kind = WalkFind(State, Level, LodFloorShift(X, Shift), LodFloorShift(Y, Shift),
                      LodFloorShift(Z, Shift));
      Transition = Kind == WALK_SELECTED;
    }
    if (Transition) {
      Faces |= 1U << (unsigned)Face;
    }
  }
  return Faces;
}

// After the walk: draws the selected sections whose mesh is current, and the
// stand-ins with whatever mesh they hold, and queues the others (prefetched
// children, which have no mesh yet, among them), now that each one's level
// transitions are known.
static void ResolveWalk(LodRendererState *State, SelectContext *Ctx) {
  for (int Idx = 0; Idx < Ctx->WalkedCount; Idx++) {
    PendingMesh Walk = State->Walked[Idx];
    Walk.Transitions = (uint8_t)TransitionFaces(State, &Walk);
    CacheEntry *Entry = CacheFind(State, Walk.Level, Walk.X, Walk.Y, Walk.Z);
    if (Entry != NULL && !Walk.Refresh) {
      Entry->LastFrame = State->Frame;
      Entry->DistSq = Walk.DistSq;
      Entry->Retained = false;
    }
    if (Walk.StandIn ||
        (!Walk.Refresh &&
         MeshCurrent(Entry, Walk.Section, Walk.Transitions, Walk.Border))) {
      AddDraw(State, Ctx, Entry, Walk.Level, Walk.X, Walk.Y, Walk.Z, Walk.Border,
              Walk.Key);
    } else if (Ctx->PendingCount < MAX_DRAWS) {
      State->PendingMeshes[Ctx->PendingCount++] = Walk;
    }
  }
}

// Nearest first; ties resolved by position so the order never depends on the
// store's hash layout.
static int ComparePending(const void *Left, const void *Right) {
  const PendingMesh *A = (const PendingMesh *)Left;
  const PendingMesh *B = (const PendingMesh *)Right;
  if (A->Refresh != B->Refresh) {
    return A->Refresh ? 1 : -1;
  }
  if (A->DistSq != B->DistSq) {
    return (A->DistSq < B->DistSq) ? -1 : 1;
  }
  int Keys[2][4] = {{A->Level, A->X, A->Y, A->Z}, {B->Level, B->X, B->Y, B->Z}};
  for (int Key = 0; Key < 4; Key++) {
    if (Keys[0][Key] != Keys[1][Key]) {
      return (Keys[0][Key] < Keys[1][Key]) ? -1 : 1;
    }
  }
  return 0;
}

// Meshes the queued sections nearest first within the frame's budget, timed
// from here (the walk before it can be long) and always allowing one attempt,
// so every pass makes progress. One that cannot be meshed now keeps drawing
// its older mesh, if it has one.
static void MeshQueued(LodRendererState *State, SelectContext *Ctx) {
  qsort(State->PendingMeshes, (size_t)Ctx->PendingCount, sizeof(PendingMesh),
        ComparePending);
  double Deadline = OsGetTimeSeconds() + MESH_BUDGET_SECONDS;
  for (int Idx = 0; Idx < Ctx->PendingCount; Idx++) {
    const PendingMesh *Pending = &State->PendingMeshes[Idx];
    CacheEntry *Entry =
        CacheFind(State, Pending->Level, Pending->X, Pending->Y, Pending->Z);
    if (Pending->Refresh) {
      // Last in the queue: only time no drawn section needed. A full arena
      // ends the refreshing instead of evicting for it.
      bool OutOfTime = Ctx->Attempts >= MESH_BUDGET_SECTIONS ||
                       (Ctx->Attempts > 0 && OsGetTimeSeconds() > Deadline);
      if (OutOfTime && !Ctx->ArenaFull) {
        Ctx->RefreshWaiting = Ctx->PendingCount - Idx;
      }
      if (OutOfTime || Ctx->ArenaFull || !MeshPending(State, Ctx, Pending, &Entry)) {
        break;
      }
      continue;
    }
    if (Ctx->ArenaFull) {
      Ctx->Starved++;
    } else if (Ctx->Attempts >= MESH_BUDGET_SECTIONS ||
               (Ctx->Attempts > 0 && OsGetTimeSeconds() > Deadline)) {
      Ctx->Waiting++;
    } else if (!MeshPending(State, Ctx, Pending, &Entry)) {
      // A full arena is not waiting: the section only gets a mesh once room
      // is freed, so it must not hold back the settled state. Nothing after
      // it is tried either, so no farther section takes the room it needs.
      Ctx->ArenaFull = true;
      Ctx->Starved++;
    } else if (Pending->Prefetch) {
      Ctx->PrefetchMeshed++;
    }
    if (Ctx->ArenaFull && Ctx->NearestStarved < 0.0F) {
      Ctx->NearestStarved = Pending->DistSq;
    }
    if (!Pending->Prefetch) {
      AddDraw(State, Ctx, Entry, Pending->Level, Pending->X, Pending->Y, Pending->Z,
              Pending->Border, Pending->Key);
    }
  }
}

static int CompareFarthest(const void *Left, const void *Right) {
  const CacheEntry *A = *(const CacheEntry *const *)Left;
  const CacheEntry *B = *(const CacheEntry *const *)Right;
  if (A->DistSq != B->DistSq) {
    return (A->DistSq > B->DistSq) ? -1 : 1;
  }
  return 0;
}

// With the arena at its ceiling, frees selected meshes farther than the
// nearest starved section, the farthest first, until Vertices are freed, so
// the room goes to what is nearer the camera. They stay in this frame's list
// (their vertices are released only frames later) and leave it at the next
// selection. Returns how many it freed.
static int EvictFarthestSelected(LodRendererState *State, int64_t Vertices,
                                 float Nearer) {
  if (Vertices <= 0 || Nearer < 0.0F) {
    return 0;
  }
  const CacheEntry **Candidates =
      (const CacheEntry **)malloc(sizeof(CacheEntry *) * (size_t)State->Capacity);
  if (Candidates == NULL) {
    return 0;
  }
  int Found = 0;
  for (int Idx = 0; Idx < State->Capacity; Idx++) {
    const CacheEntry *Entry = &State->Cache[Idx];
    if (Entry->Level > 0 && Entry->LastFrame == State->Frame &&
        Entry->Mesh != RENDER_LOD_MESH_INVALID && Entry->DistSq > Nearer) {
      Candidates[Found++] = Entry;
    }
  }
  qsort((void *)Candidates, (size_t)Found, sizeof(CacheEntry *), CompareFarthest);
  int Freed = 0;
  int64_t FreedVertices = 0;
  for (int Idx = 0; Idx < Found && FreedVertices < Vertices; Idx++) {
    CacheEntry *Entry = &State->Cache[Candidates[Idx] - State->Cache];
    FreedVertices += Entry->VertexCount;
    Freed++;
    RenderLodFreeMesh(Entry->Mesh);
    Entry->Level = SLOT_TOMBSTONE;
    State->Count--;
    State->Tombstones++;
    State->EvictedForRoom++;
  }
  free((void *)Candidates);
  return Freed;
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
  // A new anchor (camera chunk, store or coverage) restarts the eviction
  // rounds a full arena allows.
  bool Anchored = !State->HasSelection || CoverageChanged || StoreChanged || Moved;
  if (Anchored) {
    State->EvictionRounds = 0;
    State->StarvedParked = false;
  }
  // Starved sections are retried only once the arena has gained room and
  // while eviction rounds remain, so a full arena costs nothing per frame.
  bool Owed = State->Stats.WaitingSections > 0 || State->PrefetchOwed ||
              State->RefreshOwed ||
              (State->Stats.StarvedSections > 0 && !State->StarvedParked &&
               RenderLodArenaGeneration() != State->StarvedGeneration);
  if (Anchored || Owed) {
    State->Frame++;
    State->ListRevision++;
    if (State->Frame % EVICT_INTERVAL == 0U) {
      EvictStale(State, Store, (uint32_t)EVICT_AGE, false);
    }
    float Half = (float)HALF_SECTION;
    SelectContext Ctx = {
        .Store = Store,
        .Camera = {((float)ChunkX * (float)LOD_SECTION_SIZE) + Half,
                   ((float)ChunkY * (float)LOD_SECTION_SIZE) + Half,
                   ((float)ChunkZ * (float)LOD_SECTION_SIZE) + Half},
        .RefineFactor = (float)State->RefineFactor,
        .CameraChunkX = ChunkX,
        .CameraChunkZ = ChunkZ,
        .NearestStarved = -1.0F};
    WalkReset(State);
    for (int Idx = 0; Idx < Store->Capacity; Idx++) {
      const LodStoreSlot *Slot = &Store->Slots[Idx];
      if (Slot->Level == LOD_MAX_LEVEL) {
        SelectSection(State, &Ctx, LOD_MAX_LEVEL, Slot->X, Slot->Y, Slot->Z);
      }
    }
    ResolveWalk(State, &Ctx);
    MeshQueued(State, &Ctx);
    AssembleDraws(State, &Ctx);
    State->HasSelection = true;
    State->PrefetchOwed = Ctx.PrefetchMeshed > 0;
    State->RefreshOwed = Ctx.RefreshWaiting > 0;
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
      // Every mesh this pass did not select goes, then, if starved sections
      // remain nearer than selected ones, the farthest selected meshes; the
      // starved are retried once the arena's delayed frees land.
      EvictStale(State, Store, 0U, true);
      int Freed = 0;
      if (State->EvictionRounds < MAX_EVICTION_ROUNDS) {
        State->EvictionRounds++;
        Freed = EvictFarthestSelected(
            State, (int64_t)Ctx.StarvedVertices * EVICTION_SLACK, Ctx.NearestStarved);
      }
      State->StarvedParked = Freed == 0;
      State->StarvedGeneration = RenderLodArenaGeneration();
    }
  }
  RenderDrawLod(State->Draws, State->OpaqueCount, State->BorderCount,
                State->ListRevision);

  RenderGetLodStats(&State->Stats.Gpu);
  State->Stats.Enabled = true;
  State->Stats.CachedMeshes = State->Count;
  State->Stats.EvictedForRoom = State->EvictedForRoom;
  PROFILE_END(DrawStart, PROFILE_LOD_DRAW);
}

bool LodRendererIdle(void) {
  const LodRendererState *State = GetLodRendererState();
  return !State->Enabled ||
         (State->Stats.WaitingSections == 0 && !State->PrefetchOwed);
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
