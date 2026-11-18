#include "world/world.h"
#include "world/chunk.h"
#include "world/chunk_worker.h"
#include "persistence/world_save.h"
#include "core/profiler.h"
#include "core/log.h"
#include "world/worldgen_strata.h"
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DDA_MAX_CROSSED_AXES 3.0F

#define HASH_EMPTY (-1)

#define SPATIAL_PRIME_X (73856093)
#define SPATIAL_PRIME_Y (19349663)
#define SPATIAL_PRIME_Z (83492791)
 
typedef struct {
  float TMaxX, TMaxY, TMaxZ;
  float TDeltaX, TDeltaY, TDeltaZ;
  float DistanceTravelled;

  int VoxelX, VoxelY, VoxelZ;
  int StepX, StepY, StepZ;
  int LastStep;
} DdaState;

enum {
  COLUMN_CANDIDATE_SIDE = (2 * MAX_RENDER_DISTANCE) + 1,
  MAX_LOAD_LAYERS = (2 * VERTICAL_RENDER_RADIUS) + 1,
  // A band never spans more layers than this; the default world spans 74.
  MAX_BAND_LAYERS = MAX_VERTICAL_LOAD_REACH,
  MAX_COLUMN_LAYERS = MAX_LOAD_LAYERS + MAX_BAND_LAYERS,
  // Load volumes kept built: the last load request's, and one for any other
  // centre a caller asks about.
  LOAD_VOLUME_CACHE_SLOTS = 2
};

_Static_assert(((2 * NEAR_LOAD_RADIUS) + 1) * ((2 * NEAR_LOAD_RADIUS) + 1) *
                       MAX_COLUMN_LAYERS <=
                   MAX_ACTIVE_CHUNKS,
               "the near cylinder alone must always fit the slot pool");

typedef struct {
  int Dx;
  int Dz;
} ColumnOffset;

// Every chunk position the active set covers around one centre. Columns are
// offsets nearest first. The first NearCount carry the vertical window around
// the centre (Layers, ordered by distance from the centre layer, below first);
// every column carries its terrain band BandLo..BandHi, empty when
// BandLo > BandHi. The window's layers inside the world are exactly
// InWorldLo..InWorldHi (empty when InWorldLo > InWorldHi); the only others are
// the centre's neighbours outside the world, which no band reaches.
typedef struct {
  // Cache key: the request the volume answers.
  const World *Owner;
  int CenterX;
  int CenterY;
  int CenterZ;
  int Radius;
  uint32_t BoundsKey;
  uint32_t RaisedVersion;
  bool Valid;

  const ColumnOffset *Columns;
  int ColumnCount;
  int NearCount;
  int Layers[MAX_LOAD_LAYERS];
  int LayerCount;
  int WindowLo;
  int WindowHi;
  int InWorldLo;
  int InWorldHi;
  int16_t BandLo[MAX_LOADED_COLUMNS];
  int16_t BandHi[MAX_LOADED_COLUMNS];
  // Positions the volume holds, and far columns the slot budget cut down.
  int ChunkTotal;
  int TrimmedColumns;
} LoadVolume;

static int HashChunkPos(int Cx, int Cy, int Cz){
  unsigned int H = (unsigned int)((Cx * SPATIAL_PRIME_X) ^ (Cy * SPATIAL_PRIME_Y) ^ (Cz * SPATIAL_PRIME_Z));
  return (int)(H % (unsigned int)CHUNK_MAP_SIZE);
}

static int FloorDivChunkSize(int Value) {
  int Quotient = Value / CHUNK_SIZE;
  if ((Value % CHUNK_SIZE) != 0 && Value < 0) {
    Quotient--;
  }
  return Quotient;
}

static void ResolveWorldChunkRange(int *OutLo, int *OutHi) {
  int MinY = INT_MIN;
  int MaxY = INT_MAX;
  if (!GetStratumRange(STRATUM_GEN_BEDROCK_BAND, &MinY, &MaxY) &&
      !GetStratumRange(STRATUM_GEN_OVERWORLD, &MinY, &MaxY)) {
    MinY = INT_MIN;
  }
  *OutLo = FloorDivChunkSize(MinY);
  *OutHi = FloorDivChunkSize(GetGenerationCeiling());
}

int ClampRenderDistance(int RenderDist) {
  if (RenderDist < MIN_RENDER_DISTANCE) { return MIN_RENDER_DISTANCE; }
  if (RenderDist > MAX_RENDER_DISTANCE) { return MAX_RENDER_DISTANCE; }
  return RenderDist;
}

int WorldActiveChunkCount(const World *WorldVal) {
  return MAX_ACTIVE_CHUNKS - WorldVal->FreeCount;
}

DenseBlockBuffer *WorldAcquireDenseBuffer(World *WorldVal) {
  return DensePoolAcquire(&WorldVal->DensePool);
}

void WorldReleaseDenseBuffer(World *WorldVal, DenseBlockBuffer *Buffer) {
  DensePoolRelease(&WorldVal->DensePool, Buffer);
}

void WorldGetDensePoolStats(World *WorldVal, DensePoolStats *Out) {
  DensePoolGetStats(&WorldVal->DensePool, Out);
}

void WorldGetChunkStorageCounts(const World *WorldVal, ChunkStorageCounts *Out) {
  ChunkStorageCounts Counts = {0};
  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    if (!WorldVal->SlotInUse[IdxI]) {
      continue;
    }
    const Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
    if (ChunkVal->Packed != (PackedBlockBuffer *)0) {
      Counts.Packed++;
    } else if (!ChunkVal->IsUniform) {
      Counts.Dense++;
    } else if (ChunkVal->UniformBlockId == BLOCK_ID_AIR) {
      Counts.UniformAir++;
    } else {
      Counts.UniformSolid++;
    }
  }
  *Out = Counts;
}

static int CompareColumnOffsets(const void *Lhs, const void *Rhs) {
  const ColumnOffset *A = (const ColumnOffset *)Lhs;
  const ColumnOffset *B = (const ColumnOffset *)Rhs;
  int DistA = (A->Dx * A->Dx) + (A->Dz * A->Dz);
  int DistB = (B->Dx * B->Dx) + (B->Dz * B->Dz);
  if (DistA != DistB) { return (DistA < DistB) ? -1 : 1; }
  if (A->Dx != B->Dx) { return (A->Dx < B->Dx) ? -1 : 1; }
  if (A->Dz != B->Dz) { return (A->Dz < B->Dz) ? -1 : 1; }
  return 0;
}

// Every column offset of the largest disc, sorted nearest first. Ties break on
// (Dx, Dz), so the disc of any smaller radius is a prefix of the list:
// PrefixCount[R] long. Built once; the world is driven from one thread.
typedef struct {
  ColumnOffset Offsets[COLUMN_CANDIDATE_SIDE * COLUMN_CANDIDATE_SIDE];
  int PrefixCount[MAX_RENDER_DISTANCE + 1];
  bool Built;
} ColumnOrder;

static const ColumnOrder *GetColumnOrder(void) {
  static ColumnOrder Order;
  if (Order.Built) {
    return &Order;
  }
  int RadiusSq = MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE;
  int Count = 0;
  for (int Dx = -MAX_RENDER_DISTANCE; Dx <= MAX_RENDER_DISTANCE; Dx++) {
    for (int Dz = -MAX_RENDER_DISTANCE; Dz <= MAX_RENDER_DISTANCE; Dz++) {
      if ((Dx * Dx) + (Dz * Dz) <= RadiusSq) {
        Order.Offsets[Count++] = (ColumnOffset){Dx, Dz};
      }
    }
  }
  qsort(Order.Offsets, (size_t)Count, sizeof(Order.Offsets[0]),
        CompareColumnOffsets);
  // Unreachable while the disc is the one MAX_LOADED_COLUMNS counts; dropping
  // the farthest columns keeps the slot pool from ever being oversubscribed.
  if (Count > MAX_LOADED_COLUMNS) {
    Count = MAX_LOADED_COLUMNS;
  }
  int Prefix = 0;
  for (int Radius = 0; Radius <= MAX_RENDER_DISTANCE; Radius++) {
    while (Prefix < Count &&
           (Order.Offsets[Prefix].Dx * Order.Offsets[Prefix].Dx) +
                   (Order.Offsets[Prefix].Dz * Order.Offsets[Prefix].Dz) <=
               Radius * Radius) {
      Prefix++;
    }
    Order.PrefixCount[Radius] = Prefix;
  }
  Order.Built = true;
  return &Order;
}

enum {
  // Underground, each layer the centre sits below the first layer under the
  // sea-level chunk moves this many window layers from above it to below it.
  DEPTH_SHIFT_PER_LAYER = 1,
  // Layers the window keeps above the centre however deep it is, so a cavern's
  // ceiling stays loaded.
  MIN_UP_LAYERS = 2
};

_Static_assert(MAX_LOAD_LAYERS == MAX_LOAD_LAYER_REACH + 2,
               "a window of MAX_LOAD_LAYERS layers holding the layer above the "
               "centre reaches at most MAX_LOAD_LAYER_REACH below it");

// The 2V + 1 layer window around CenterY. Underground it is biased downward,
// where the visible depth is; then it slides off the generation ceiling and the
// bedrock floor instead of being cut there, so layers outside the world are
// spent inside it. Each slide stops one layer short of the centre, so the
// window always holds CenterY - 1 .. CenterY + 1.
static void ComputeLayerWindow(int CenterY, int Vertical, int WorldLo,
                               int WorldHi, int *OutLo, int *OutHi) {
  int Depth = (FloorDivChunkSize(SEA_LEVEL) - 1) - CenterY;
  if (Depth > Vertical) { Depth = Vertical; }
  int Shift = Depth * DEPTH_SHIFT_PER_LAYER;
  if (Shift > Vertical - MIN_UP_LAYERS) { Shift = Vertical - MIN_UP_LAYERS; }
  if (Shift < 0) { Shift = 0; }

  int Lo = CenterY - (Vertical + Shift);
  int Hi = CenterY + (Vertical - Shift);

  if (Hi > WorldHi) {
    int Move = Hi - WorldHi;
    int MaxMove = (Hi - CenterY) - 1;
    if (Move > MaxMove) { Move = MaxMove; }
    Lo -= Move;
    Hi -= Move;
  }
  if (Lo < WorldLo) {
    int Move = WorldLo - Lo;
    int MaxMove = (CenterY - Lo) - 1;
    if (Move > MaxMove) { Move = MaxMove; }
    Lo += Move;
    Hi += Move;
  }

  *OutLo = Lo;
  *OutHi = Hi;
}

static int ClampInt(int Value, int Lo, int Hi) {
  if (Value < Lo) { return Lo; }
  if (Value > Hi) { return Hi; }
  return Value;
}

static bool ColumnIsNear(const LoadVolume *Volume, int Col) {
  return Col < Volume->NearCount;
}

// Positions column Col holds: its window layers when near, united with its
// band. A band lies inside the world, so it meets the window only there.
static int ColumnChunkCount(const LoadVolume *Volume, int Col) {
  int BandLo = Volume->BandLo[Col];
  int BandHi = Volume->BandHi[Col];
  int Count = (BandHi >= BandLo) ? BandHi - BandLo + 1 : 0;
  if (ColumnIsNear(Volume, Col)) {
    Count += Volume->LayerCount;
    int OverlapLo = (BandLo > Volume->InWorldLo) ? BandLo : Volume->InWorldLo;
    int OverlapHi = (BandHi < Volume->InWorldHi) ? BandHi : Volume->InWorldHi;
    if (OverlapHi >= OverlapLo) { Count -= OverlapHi - OverlapLo + 1; }
  }
  return Count;
}

// Column Col's layers in load order: a near column's window by distance from
// the centre, then the rest of its band from the top down, so the surface a
// far column shows comes before the rock under it. Out holds at least
// MAX_COLUMN_LAYERS entries.
static int ColumnLayers(const LoadVolume *Volume, int Col, int *Out) {
  int Count = 0;
  bool Near = ColumnIsNear(Volume, Col);
  if (Near) {
    for (int Layer = 0; Layer < Volume->LayerCount; Layer++) {
      Out[Count++] = Volume->Layers[Layer];
    }
  }
  for (int Y = Volume->BandHi[Col]; Y >= Volume->BandLo[Col]; Y--) {
    if (Near && Y >= Volume->InWorldLo && Y <= Volume->InWorldHi) {
      continue;
    }
    Out[Count++] = Y;
  }
  return Count;
}

// The slot pool bounds the whole volume. Unreachable with today's terrain
// (bands hold about 80k positions at the largest radius), but a band is only
// bounded by the world's height: past the budget, far columns keep just their
// top layer, the one seen from afar, and then lose their band, farthest first.
static void TrimToSlotBudget(LoadVolume *Volume) {
  for (int Pass = 0; Pass < 2 && Volume->ChunkTotal > MAX_ACTIVE_CHUNKS;
       Pass++) {
    for (int Col = Volume->ColumnCount - 1;
         Col >= Volume->NearCount && Volume->ChunkTotal > MAX_ACTIVE_CHUNKS;
         Col--) {
      if (Volume->BandHi[Col] < Volume->BandLo[Col]) { continue; }
      Volume->ChunkTotal -= ColumnChunkCount(Volume, Col);
      if (Pass == 0) {
        Volume->BandLo[Col] = Volume->BandHi[Col];
        Volume->TrimmedColumns++;
      } else {
        Volume->BandLo[Col] = (int16_t)(Volume->BandHi[Col] + 1);
      }
      Volume->ChunkTotal += ColumnChunkCount(Volume, Col);
    }
  }
}

// The window's layers are (the window intersected with the world's range)
// united with [Cy - 1, Cy + 1], so the chunks around the centre stay loaded
// even outside the world. Both parts lie inside the window, which bounds the
// layer count by MAX_LOAD_LAYERS. Layers are listed by distance from the
// centre, below first.
static void BuildWindowLayers(const World *WorldVal, int CenterY, int Vertical,
                              LoadVolume *Out) {
  int Lo = 0;
  int Hi = 0;
  ComputeLayerWindow(CenterY, Vertical, WorldVal->WorldLoChunkY,
                     WorldVal->WorldHiChunkY, &Lo, &Hi);
  if (Lo < WorldVal->WorldLoChunkY) { Lo = WorldVal->WorldLoChunkY; }
  if (Hi > WorldVal->WorldHiChunkY) { Hi = WorldVal->WorldHiChunkY; }
  Out->InWorldLo = Lo;
  Out->InWorldHi = Hi;

  Out->LayerCount = 0;
  Out->WindowLo = CenterY;
  Out->WindowHi = CenterY;
  for (int Step = 0; Step <= MAX_LOAD_LAYER_REACH; Step++) {
    int Below = CenterY - Step;
    int Above = CenterY + Step;
    if ((Below >= Lo && Below <= Hi) || Step <= 1) {
      Out->Layers[Out->LayerCount++] = Below;
      Out->WindowLo = Below;
    }
    if (Step > 0 && ((Above >= Lo && Above <= Hi) || Step <= 1)) {
      Out->Layers[Out->LayerCount++] = Above;
      Out->WindowHi = Above;
    }
  }
}

// A column's band: from the lowest layer terrain air can reach up to the
// highest layer holding anything, terrain or player edits, clamped to the
// world.
static void ResolveColumnBand(World *WorldVal, int ColumnX, int ColumnZ,
                              int16_t *OutLo, int16_t *OutHi) {
  ColumnBoundsTable *Table = &WorldVal->ColumnBounds;
  int Top = ColumnBoundsTopLayer(Table, ColumnX, ColumnZ);
  int Bottom = ColumnBoundsBottomLayer(Table, ColumnX, ColumnZ);
  int Raised = 0;
  if (RaisedTopsGet(&WorldVal->RaisedTops, ColumnX, ColumnZ, &Raised) &&
      Raised > Top) {
    Top = Raised;
  }
  Top = ClampInt(Top, WorldVal->WorldLoChunkY, WorldVal->WorldHiChunkY);
  Bottom = ClampInt(Bottom, WorldVal->WorldLoChunkY, Top);
  if (Top - Bottom + 1 > MAX_BAND_LAYERS) { Bottom = Top - MAX_BAND_LAYERS + 1; }
  *OutLo = (int16_t)Bottom;
  *OutHi = (int16_t)Top;
}

static void BuildLoadVolume(World *WorldVal, int CenterX, int CenterY,
                            int CenterZ, int Radius, LoadVolume *Out) {
  const ColumnOrder *Order = GetColumnOrder();
  int Vertical = (Radius < VERTICAL_RENDER_RADIUS) ? Radius
                                                   : VERTICAL_RENDER_RADIUS;
  int NearRadius = (Radius < NEAR_LOAD_RADIUS) ? Radius : NEAR_LOAD_RADIUS;

  Out->Owner = WorldVal;
  Out->CenterX = CenterX;
  Out->CenterY = CenterY;
  Out->CenterZ = CenterZ;
  Out->Radius = Radius;
  Out->BoundsKey = WorldVal->ColumnBounds.LayerKey;
  Out->RaisedVersion = WorldVal->RaisedTops.Version;
  Out->Columns = Order->Offsets;
  Out->ColumnCount = Order->PrefixCount[Radius];
  Out->NearCount = Order->PrefixCount[NearRadius];
  BuildWindowLayers(WorldVal, CenterY, Vertical, Out);

  // A window above a near column's band leaves air between them: the band
  // stops at the highest layer that can hold anything. Absent chunks inside a
  // column read as rock, so the band is extended up to the window instead, or
  // to the world's top when the window lies above the world. A window below
  // the band (underground) leaves rock between them, which may stay absent.
  int FillTo = INT_MIN;
  if (Out->InWorldLo <= Out->InWorldHi) {
    FillTo = Out->InWorldLo - 1;
  } else if (Out->WindowLo > WorldVal->WorldHiChunkY) {
    FillTo = WorldVal->WorldHiChunkY;
  }

  Out->ChunkTotal = 0;
  Out->TrimmedColumns = 0;
  for (int Col = 0; Col < Out->ColumnCount; Col++) {
    ResolveColumnBand(WorldVal, CenterX + Out->Columns[Col].Dx,
                      CenterZ + Out->Columns[Col].Dz, &Out->BandLo[Col],
                      &Out->BandHi[Col]);
    if (ColumnIsNear(Out, Col) && FillTo > Out->BandHi[Col]) {
      Out->BandHi[Col] = (int16_t)FillTo;
      if (Out->BandHi[Col] - Out->BandLo[Col] + 1 > MAX_BAND_LAYERS) {
        Out->BandLo[Col] = (int16_t)(Out->BandHi[Col] - MAX_BAND_LAYERS + 1);
      }
    }
    Out->ChunkTotal += ColumnChunkCount(Out, Col);
  }
  TrimToSlotBudget(Out);
  Out->Valid = true;
}

static bool VolumeAnswers(const LoadVolume *Volume, const World *WorldVal,
                          int CenterX, int CenterY, int CenterZ, int Radius) {
  return Volume->Valid && Volume->Owner == WorldVal &&
         Volume->CenterX == CenterX && Volume->CenterY == CenterY &&
         Volume->CenterZ == CenterZ && Volume->Radius == Radius &&
         Volume->BoundsKey == WorldVal->ColumnBounds.LayerKey &&
         Volume->RaisedVersion == WorldVal->RaisedTops.Version;
}

// The volume for a centre chunk and render distance, built on demand and kept:
// the bounds are pure and the raised tops versioned, so a kept volume stays
// exact until its key changes. The pointer is valid until a call with another
// key. Main thread only.
static const LoadVolume *GetLoadVolumeAt(World *WorldVal, int CenterX,
                                         int CenterY, int CenterZ,
                                         int RenderDist) {
  static LoadVolume Cache[LOAD_VOLUME_CACHE_SLOTS];
  static int NextSlot = 0;
  int Radius = ClampRenderDistance(RenderDist);
  for (int Slot = 0; Slot < LOAD_VOLUME_CACHE_SLOTS; Slot++) {
    if (VolumeAnswers(&Cache[Slot], WorldVal, CenterX, CenterY, CenterZ,
                      Radius)) {
      return &Cache[Slot];
    }
  }
  LoadVolume *Volume = &Cache[NextSlot];
  NextSlot = (NextSlot + 1) % LOAD_VOLUME_CACHE_SLOTS;
  BuildLoadVolume(WorldVal, CenterX, CenterY, CenterZ, Radius, Volume);
  return Volume;
}

static const LoadVolume *GetLoadVolume(World *WorldVal, Vec3 Center,
                                       int RenderDist) {
  return GetLoadVolumeAt(WorldVal, (int)__builtin_floorf(Center.x / CHUNK_SIZE),
                         (int)__builtin_floorf(Center.y / CHUNK_SIZE),
                         (int)__builtin_floorf(Center.z / CHUNK_SIZE),
                         RenderDist);
}

#ifdef MINEGAME_PROFILE
static bool WindowIsValid(const LoadVolume *Volume, int Vertical) {
  if (Volume->LayerCount > (2 * Vertical) + 1) { return false; }
  int NearSeen = 0;
  int PrevDistance = 0;
  for (int Idx = 0; Idx < Volume->LayerCount; Idx++) {
    int Layer = Volume->Layers[Idx];
    int Distance = abs(Layer - Volume->CenterY);
    if (Distance > MAX_LOAD_LAYER_REACH || Distance < PrevDistance) {
      return false;
    }
    PrevDistance = Distance;
    for (int Other = 0; Other < Idx; Other++) {
      if (Volume->Layers[Other] == Layer) { return false; }
    }
    if (Layer < Volume->WindowLo || Layer > Volume->WindowHi) { return false; }
    bool InWorld = Layer >= Volume->InWorldLo && Layer <= Volume->InWorldHi;
    if (!InWorld && Distance > 1) { return false; }
    if (Distance <= 1) { NearSeen++; }
  }
  // Every in-world layer of the window is listed.
  for (int Y = Volume->InWorldLo; Y <= Volume->InWorldHi; Y++) {
    bool Listed = false;
    for (int Idx = 0; Idx < Volume->LayerCount; Idx++) {
      Listed = Listed || Volume->Layers[Idx] == Y;
    }
    if (!Listed) { return false; }
  }
  return NearSeen == 3;
}

// Bands inside the world and within the band limit, each column's layer list
// free of duplicates and as long as ColumnChunkCount says, and the whole
// within the slot pool.
static bool BandsAreValid(const World *WorldVal, const LoadVolume *Volume) {
  int Layers[MAX_COLUMN_LAYERS];
  int Total = 0;
  for (int Col = 0; Col < Volume->ColumnCount; Col++) {
    int Lo = Volume->BandLo[Col];
    int Hi = Volume->BandHi[Col];
    if (Hi >= Lo && (Lo < WorldVal->WorldLoChunkY ||
                     Hi > WorldVal->WorldHiChunkY ||
                     Hi - Lo + 1 > MAX_BAND_LAYERS)) {
      return false;
    }
    int Count = ColumnLayers(Volume, Col, Layers);
    if (Count != ColumnChunkCount(Volume, Col)) { return false; }
    for (int Idx = 0; Idx < Count; Idx++) {
      for (int Other = 0; Other < Idx; Other++) {
        if (Layers[Other] == Layers[Idx]) { return false; }
      }
    }
    Total += Count;
  }
  return Total == Volume->ChunkTotal && Total <= MAX_ACTIVE_CHUNKS;
}

typedef struct {
  int CenterY;
  int Lo;
  int Hi;
} WindowExpectation;

static bool IsBandCheckRadius(int Radius) {
  return Radius == MIN_RENDER_DISTANCE || Radius == DEFAULT_RENDER_DISTANCE ||
         Radius == NEAR_LOAD_RADIUS + 1 || Radius == MAX_RENDER_DISTANCE;
}

bool WorldCheckLoadVolumes(World *WorldVal) {
  enum { SWEEP_LO = -40, SWEEP_HI = 70, SPEC_WORLD_LO = -33, SPEC_WORLD_HI = 40 };
  int Failures = 0;

  // Bands depend on the radius only through how many columns there are, so
  // they are checked at a few radii; the window at every one.
  for (int Radius = MIN_RENDER_DISTANCE; Radius <= MAX_RENDER_DISTANCE; Radius++) {
    int Vertical = (Radius < VERTICAL_RENDER_RADIUS) ? Radius
                                                     : VERTICAL_RENDER_RADIUS;
    bool CheckBands = IsBandCheckRadius(Radius);
    for (int CenterY = SWEEP_LO; CenterY <= SWEEP_HI; CenterY++) {
      // Static: a volume is too large for the stack.
      static LoadVolume WindowOnly;
      const LoadVolume *Volume = &WindowOnly;
      if (CheckBands) {
        Volume = GetLoadVolumeAt(WorldVal, 0, CenterY, 0, Radius);
      } else {
        WindowOnly.CenterY = CenterY;
        BuildWindowLayers(WorldVal, CenterY, Vertical, &WindowOnly);
      }
      if (!WindowIsValid(Volume, Vertical) ||
          (CheckBands && !BandsAreValid(WorldVal, Volume))) {
        LogError("WORLD: load volume invalid at chunk Y %d, R %d", CenterY,
                 Radius);
        Failures++;
      }
    }
  }

  // The worked values of the cylindrical-chunk-loading spec, which assume the
  // default strata.
  if (WorldVal->WorldLoChunkY == SPEC_WORLD_LO &&
      WorldVal->WorldHiChunkY == SPEC_WORLD_HI) {
    static const WindowExpectation EXPECTED[] = {
        {4, -4, 12},    {-1, -9, 7},  {-8, -22, -6},
        {-32, -33, -17}, {38, 24, 40}, {60, 59, 61},
    };
    for (size_t Idx = 0; Idx < sizeof(EXPECTED) / sizeof(EXPECTED[0]); Idx++) {
      const WindowExpectation *Case = &EXPECTED[Idx];
      const LoadVolume *Volume = GetLoadVolumeAt(
          WorldVal, 0, Case->CenterY, 0, DEFAULT_RENDER_DISTANCE);
      if (Volume->WindowLo != Case->Lo || Volume->WindowHi != Case->Hi) {
        LogError("WORLD: chunk Y %d loads %d..%d, expected %d..%d",
                 Case->CenterY, Volume->WindowLo, Volume->WindowHi, Case->Lo,
                 Case->Hi);
        Failures++;
      }
    }
  }

  if (Failures == 0) {
    LogInfo("WORLD: load volumes valid for chunk Y %d..%d, R %d..%d",
            SWEEP_LO, SWEEP_HI, MIN_RENDER_DISTANCE, MAX_RENDER_DISTANCE);
  }
  return Failures == 0;
}
#endif

static void RaiseSavedChunk(void *UserData, int ChunkX, int ChunkY,
                            int ChunkZ) {
  World *WorldVal = (World *)UserData;
  (void)RaisedTopsRaise(&WorldVal->RaisedTops, ChunkX, ChunkZ, ChunkY);
}

void InitWorld(World *WorldVal){
  WorldVal->ChunkCount = 0;

  for(int IdxI = 0; IdxI < CHUNK_MAP_SIZE; IdxI++){
    WorldVal->ChunkHashMap[IdxI] = HASH_EMPTY;
  }

  // Stacked so the lowest index pops first: ChunkCount then tracks how many
  // slots the chosen render distance actually needs, not the whole pool.
  WorldVal->FreeCount = MAX_ACTIVE_CHUNKS;
  for(int IdxI = 0; IdxI < MAX_ACTIVE_CHUNKS; IdxI++){
    WorldVal->FreeList[IdxI] = MAX_ACTIVE_CHUNKS - 1 - IdxI;
    WorldVal->SlotInUse[IdxI] = false;
    WorldVal->CandBucket[IdxI] = -1;
  }
  for (int Bucket = 0; Bucket < MESH_CANDIDATE_BUCKETS; Bucket++) {
    WorldVal->CandHead[Bucket] = -1;
  }
  WorldVal->CandidateCount = 0;
  WorldVal->CandCenterX = 0;
  WorldVal->CandCenterY = 0;
  WorldVal->CandCenterZ = 0;

  DensePoolInit(&WorldVal->DensePool);

  WorldVal->LastLoadChunkX = 0;
  WorldVal->LastLoadChunkY = 0;
  WorldVal->LastLoadChunkZ = 0;
  WorldVal->LastLoadRenderDist = 0;
  WorldVal->HasLoadedOnce = false;
  WorldVal->LastLoadAllCreated = false;
  WorldVal->PendingGeneration = 0;
#ifdef MINEGAME_PROFILE
  WorldVal->LastGenerationSettled = false;
  WorldVal->LastSettled = false;
#endif
  ResolveWorldChunkRange(&WorldVal->WorldLoChunkY, &WorldVal->WorldHiChunkY);
  ColumnBoundsInit(&WorldVal->ColumnBounds);
  for (int Slot = 0; Slot < LOADED_COLUMN_HASH_SIZE; Slot++) {
    WorldVal->LoadedColumns[Slot].Used = false;
  }
  RaisedTopsInit(&WorldVal->RaisedTops);
  // Saved chunks are the player's edits: a load range must reach them even
  // where the terrain bounds say the column is empty.
  WorldSaveForEachSavedChunk(RaiseSavedChunk, WorldVal);
  WorldVal->OnChunkEvict = (ChunkEvictCallback)0;
  WorldVal->OnBlockChange = (BlockChangeCallback)0;
  WorldVal->BlockChangeUserData = NULL;
}

void DestroyWorld(World *WorldVal) {
  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    if (WorldVal->SlotInUse[IdxI]) {
      ChunkResetStorage(&WorldVal->Chunks[IdxI], BLOCK_ID_AIR);
    }
  }
  DensePoolDestroy(&WorldVal->DensePool);
  RaisedTopsFree(&WorldVal->RaisedTops);
}

void WorldSetChunkEvictCallback(World *WorldVal, ChunkEvictCallback Callback) {
  WorldVal->OnChunkEvict = Callback;
}

void WorldSetBlockChangeCallback(World *WorldVal, BlockChangeCallback Callback,
                                 void *UserData) {
  WorldVal->OnBlockChange = Callback;
  WorldVal->BlockChangeUserData = UserData;
}

static int HashChunkSlot(const World *WorldVal, int ChunkIndex) {
  const Chunk *ChunkVal = &WorldVal->Chunks[ChunkIndex];
  return HashChunkPos(ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
}

static bool ChunkIsAt(const World *WorldVal, int ChunkIndex, int ChunkX,
                      int ChunkY, int ChunkZ) {
  const Chunk *ChunkVal = &WorldVal->Chunks[ChunkIndex];
  return ChunkVal->ChunkX == ChunkX && ChunkVal->ChunkY == ChunkY &&
         ChunkVal->ChunkZ == ChunkZ;
}

// Cyclic number of probe steps from slot From forward to slot To.
static int ProbeDistance(int From, int To) {
  return (To - From + CHUNK_MAP_SIZE) % CHUNK_MAP_SIZE;
}

// ---------------------------------------------------------------------------
// Loaded columns
// ---------------------------------------------------------------------------

enum {
  // Spatial-hash primes and the fold that brings the high bits down.
  LOADED_COLUMN_PRIME_X = 73856093,
  LOADED_COLUMN_PRIME_Z = 19349663,
  LOADED_COLUMN_FOLD_SHIFT = 15
};

static int LoadedColumnHome(int ColumnX, int ColumnZ) {
  unsigned int Hash = ((unsigned int)ColumnX * (unsigned int)LOADED_COLUMN_PRIME_X) ^
                      ((unsigned int)ColumnZ * (unsigned int)LOADED_COLUMN_PRIME_Z);
  Hash ^= Hash >> (unsigned int)LOADED_COLUMN_FOLD_SHIFT;
  return (int)(Hash & (unsigned int)LOADED_COLUMN_HASH_MASK);
}

// The column's slot, or -1 when it holds nothing.
static int FindLoadedColumn(const World *WorldVal, int ColumnX, int ColumnZ) {
  int Slot = LoadedColumnHome(ColumnX, ColumnZ);
  while (WorldVal->LoadedColumns[Slot].Used) {
    const LoadedColumn *Entry = &WorldVal->LoadedColumns[Slot];
    if (Entry->ColumnX == ColumnX && Entry->ColumnZ == ColumnZ) {
      return Slot;
    }
    Slot = (Slot + 1) & LOADED_COLUMN_HASH_MASK;
  }
  return -1;
}

static void LoadedColumnAdd(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ) {
  int Slot = LoadedColumnHome(ChunkX, ChunkZ);
  while (WorldVal->LoadedColumns[Slot].Used) {
    LoadedColumn *Entry = &WorldVal->LoadedColumns[Slot];
    if (Entry->ColumnX == ChunkX && Entry->ColumnZ == ChunkZ) {
      Entry->Count++;
      // A stale top is only ever too high, so a layer above it is the new top
      // whatever the rescan would have found.
      if (ChunkY > Entry->Top) {
        Entry->Top = (int16_t)ChunkY;
        Entry->TopStale = false;
      }
      return;
    }
    Slot = (Slot + 1) & LOADED_COLUMN_HASH_MASK;
  }
  WorldVal->LoadedColumns[Slot] = (LoadedColumn){.ColumnX = ChunkX,
                                                 .ColumnZ = ChunkZ,
                                                 .Top = (int16_t)ChunkY,
                                                 .Count = 1,
                                                 .Used = true};
}

// Backward-shift deletion, as the chunk map does.
static void DeleteLoadedColumn(World *WorldVal, int Hole) {
  WorldVal->LoadedColumns[Hole].Used = false;
  int Current = (Hole + 1) & LOADED_COLUMN_HASH_MASK;
  while (WorldVal->LoadedColumns[Current].Used) {
    const LoadedColumn *Moving = &WorldVal->LoadedColumns[Current];
    int Home = LoadedColumnHome(Moving->ColumnX, Moving->ColumnZ);
    int FromHome = (Current - Home) & LOADED_COLUMN_HASH_MASK;
    int FromHole = (Current - Hole) & LOADED_COLUMN_HASH_MASK;
    if (FromHome >= FromHole) {
      WorldVal->LoadedColumns[Hole] = *Moving;
      WorldVal->LoadedColumns[Current].Used = false;
      Hole = Current;
    }
    Current = (Current + 1) & LOADED_COLUMN_HASH_MASK;
  }
}

static void LoadedColumnRemove(World *WorldVal, int ChunkX, int ChunkY,
                               int ChunkZ) {
  int Slot = FindLoadedColumn(WorldVal, ChunkX, ChunkZ);
  if (Slot < 0) { return; }
  LoadedColumn *Entry = &WorldVal->LoadedColumns[Slot];
  Entry->Count--;
  if (Entry->Count == 0) {
    DeleteLoadedColumn(WorldVal, Slot);
  } else if (ChunkY >= Entry->Top) {
    Entry->TopStale = true;
  }
}

bool WorldGetLoadedColumnTop(World *WorldVal, int ColumnX, int ColumnZ,
                             int *OutTop) {
  int Slot = FindLoadedColumn(WorldVal, ColumnX, ColumnZ);
  if (Slot < 0) { return false; }
  LoadedColumn *Entry = &WorldVal->LoadedColumns[Slot];
  // Every remaining chunk of the column lies below a stale top, and Count says
  // one does, so the downward scan ends.
  if (Entry->TopStale) {
    int Y = Entry->Top - 1;
    while (GetChunkFromWorld(WorldVal, ColumnX, Y, ColumnZ) == (Chunk *)0) {
      Y--;
    }
    Entry->Top = (int16_t)Y;
    Entry->TopStale = false;
  }
  *OutTop = Entry->Top;
  return true;
}

// The map is at most 25% full (CHUNK_MAP_SIZE = 4 * MAX_ACTIVE_CHUNKS), so
// every probe sequence reaches an empty slot and these loops terminate.
static void InsertChunkIntoMap(World *WorldVal, int ChunkIndex) {
  int Hash = HashChunkSlot(WorldVal, ChunkIndex);

  while(WorldVal->ChunkHashMap[Hash] != HASH_EMPTY){
    Hash = (Hash + 1) % CHUNK_MAP_SIZE;
  }
  WorldVal->ChunkHashMap[Hash] = ChunkIndex;
  const Chunk *ChunkVal = &WorldVal->Chunks[ChunkIndex];
  LoadedColumnAdd(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
}

// Backward-shift deletion: rather than leaving a tombstone, entries after the
// hole that probed past it are moved back into it, so the table ends up exactly
// as if the removed chunk had never been inserted. An entry may fill the hole
// only when the hole lies on its probe path, i.e. between its home slot and its
// current slot (cyclically); otherwise moving it would put it before its home
// and lookups starting there would miss it.
static void RemoveChunkFromMap(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ) {
  int Hole = HashChunkPos(ChunkX, ChunkY, ChunkZ);

  for(;;) {
    int Idx = WorldVal->ChunkHashMap[Hole];
    if(Idx == HASH_EMPTY) { return; }
    if(ChunkIsAt(WorldVal, Idx, ChunkX, ChunkY, ChunkZ)) { break; }
    Hole = (Hole + 1) % CHUNK_MAP_SIZE;
  }

  int Next = (Hole + 1) % CHUNK_MAP_SIZE;
  while(WorldVal->ChunkHashMap[Next] != HASH_EMPTY) {
    int Idx = WorldVal->ChunkHashMap[Next];
    int Home = HashChunkSlot(WorldVal, Idx);
    if(ProbeDistance(Home, Next) >= ProbeDistance(Hole, Next)) {
      WorldVal->ChunkHashMap[Hole] = Idx;
      Hole = Next;
    }
    Next = (Next + 1) % CHUNK_MAP_SIZE;
  }
  WorldVal->ChunkHashMap[Hole] = HASH_EMPTY;
  LoadedColumnRemove(WorldVal, ChunkX, ChunkY, ChunkZ);
}

Chunk* GetChunkFromWorld(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ){
  int Hash = HashChunkPos(ChunkX, ChunkY, ChunkZ);

  while(WorldVal->ChunkHashMap[Hash] != HASH_EMPTY) {
    int Idx = WorldVal->ChunkHashMap[Hash];
    if(ChunkIsAt(WorldVal, Idx, ChunkX, ChunkY, ChunkZ)) {
      return &WorldVal->Chunks[Idx];
    }
    Hash = (Hash + 1) % CHUNK_MAP_SIZE;
  }
  return (Chunk *)0;
}

Chunk* GetChunkAtPos(World *WorldVal, Vec3 Pos){
  int ChunkX = (int)__builtin_floorf(Pos.x / CHUNK_SIZE);
  int ChunkY = (int)__builtin_floorf(Pos.y / CHUNK_SIZE);
  int ChunkZ = (int)__builtin_floorf(Pos.z / CHUNK_SIZE);

  return GetChunkFromWorld(WorldVal, ChunkX, ChunkY, ChunkZ);
}

static void MarkUsefulChunks(World *WorldVal, const LoadVolume *Volume, bool *KeepChunk){
  for(int Col = 0; Col < Volume->ColumnCount; Col++){
    int ChunkX = Volume->CenterX + Volume->Columns[Col].Dx;
    int ChunkZ = Volume->CenterZ + Volume->Columns[Col].Dz;
    int Layers[MAX_COLUMN_LAYERS];
    int LayerCount = ColumnLayers(Volume, Col, Layers);
    for(int Layer = 0; Layer < LayerCount; Layer++){
      Chunk* ChunkVal = GetChunkFromWorld(WorldVal, ChunkX, Layers[Layer], ChunkZ);
      if(ChunkVal != (Chunk *)0){
        KeepChunk[ChunkVal - WorldVal->Chunks] = true;
      }
    }
  }
}

static int MeshCandidateBucket(const World *WorldVal, const Chunk *ChunkVal) {
  int Dx = ChunkVal->ChunkX - WorldVal->CandCenterX;
  int Dy = ChunkVal->ChunkY - WorldVal->CandCenterY;
  int Dz = ChunkVal->ChunkZ - WorldVal->CandCenterZ;
  // A quarter weight on the vertical keeps every loaded layer of the nearest
  // columns ahead of the horizon, while nearer layers still go first.
  int DistanceSq = (Dx * Dx) + (Dz * Dz) + ((Dy * Dy) / 4);
  int Bucket = (int)sqrtf((float)DistanceSq);
  return (Bucket < MESH_CANDIDATE_BUCKETS) ? Bucket : MESH_CANDIDATE_BUCKETS - 1;
}

static void LinkMeshCandidate(World *WorldVal, int Slot, int Bucket) {
  int Head = WorldVal->CandHead[Bucket];
  WorldVal->CandBucket[Slot] = (int16_t)Bucket;
  WorldVal->CandPrev[Slot] = -1;
  WorldVal->CandNext[Slot] = Head;
  if (Head >= 0) {
    WorldVal->CandPrev[Head] = Slot;
  }
  WorldVal->CandHead[Bucket] = Slot;
}

static void UnlinkMeshCandidate(World *WorldVal, int Slot) {
  int Prev = WorldVal->CandPrev[Slot];
  int Next = WorldVal->CandNext[Slot];
  if (Prev >= 0) {
    WorldVal->CandNext[Prev] = Next;
  } else {
    WorldVal->CandHead[WorldVal->CandBucket[Slot]] = Next;
  }
  if (Next >= 0) {
    WorldVal->CandPrev[Next] = Prev;
  }
  WorldVal->CandBucket[Slot] = -1;
}

// A chunk still generating is left out: its completion adds it, so the
// queueing pass never revisits chunks that cannot be meshed yet.
static void AddMeshCandidate(World *WorldVal, Chunk *ChunkVal) {
  int Slot = (int)(ChunkVal - WorldVal->Chunks);
  if (!ChunkVal->IsGenerated || WorldVal->CandBucket[Slot] >= 0) {
    return;
  }
  LinkMeshCandidate(WorldVal, Slot, MeshCandidateBucket(WorldVal, ChunkVal));
  WorldVal->CandidateCount++;
}

void WorldRemoveMeshCandidate(World *WorldVal, int Slot) {
  if (WorldVal->CandBucket[Slot] < 0) {
    return;
  }
  UnlinkMeshCandidate(WorldVal, Slot);
  WorldVal->CandidateCount--;
}

int WorldNextMeshCandidate(const World *WorldVal, int Slot) {
  int Bucket = 0;
  if (Slot >= 0) {
    if (WorldVal->CandNext[Slot] >= 0) {
      return WorldVal->CandNext[Slot];
    }
    Bucket = WorldVal->CandBucket[Slot] + 1;
  }
  for (; Bucket < MESH_CANDIDATE_BUCKETS; Bucket++) {
    if (WorldVal->CandHead[Bucket] >= 0) {
      return WorldVal->CandHead[Bucket];
    }
  }
  return -1;
}

int WorldMeshCandidateCount(const World *WorldVal) {
  return WorldVal->CandidateCount;
}

// Buckets are distances from the load centre, so they go stale when it moves.
// Recomputed here, once per centre change, rather than sorted every frame.
static void RebucketMeshCandidates(World *WorldVal, int CenterX, int CenterY,
                                   int CenterZ) {
  if (WorldVal->CandCenterX == CenterX && WorldVal->CandCenterY == CenterY &&
      WorldVal->CandCenterZ == CenterZ) {
    return;
  }
  WorldVal->CandCenterX = CenterX;
  WorldVal->CandCenterY = CenterY;
  WorldVal->CandCenterZ = CenterZ;

  // Static: one entry per active chunk is too large for the stack, and the
  // world is driven from one thread.
  static int Slots[MAX_ACTIVE_CHUNKS];
  int Count = 0;
  for (int Bucket = 0; Bucket < MESH_CANDIDATE_BUCKETS; Bucket++) {
    for (int Slot = WorldVal->CandHead[Bucket]; Slot >= 0;
         Slot = WorldVal->CandNext[Slot]) {
      Slots[Count++] = Slot;
    }
    WorldVal->CandHead[Bucket] = -1;
  }
  for (int Idx = 0; Idx < Count; Idx++) {
    int Slot = Slots[Idx];
    LinkMeshCandidate(WorldVal, Slot,
                      MeshCandidateBucket(WorldVal, &WorldVal->Chunks[Slot]));
  }
}

// A mesh already in flight was built from a snapshot taken before this change,
// so it is stale. Completion clears IsDirty only when DirtiedWhileMeshing is
// unset; setting IsDirty alone would be overwritten and the chunk would keep a
// mesh built against a neighbour that has since arrived, left or changed.
static void MarkChunkDirty(World *WorldVal, Chunk *ChunkVal) {
  if (ChunkVal == (Chunk *)0) {
    return;
  }
  ChunkVal->IsDirty = true;
  if (ChunkVal->IsMeshing) { ChunkVal->DirtiedWhileMeshing = true; }
  AddMeshCandidate(WorldVal, ChunkVal);
}

// The mesh snapshot reads all 26 surrounding chunks, not only the six sharing a
// face: ambient occlusion reaches diagonally across chunk edges and corners,
// and tint samples the neighbouring biome cells. Every chunk whose mesh a
// change can reach therefore has to be marked, or one meshed while an edge or
// corner neighbour was missing keeps that stale shading forever.
void UpdateNeighborsDirtyFlag(World *WorldVal, int Cx, int Cy, int Cz){
  for (int Dx = -1; Dx <= 1; Dx++) {
    for (int Dy = -1; Dy <= 1; Dy++) {
      for (int Dz = -1; Dz <= 1; Dz++) {
        if (Dx != 0 || Dy != 0 || Dz != 0) {
          MarkChunkDirty(WorldVal,
                         GetChunkFromWorld(WorldVal, Cx + Dx, Cy + Dy, Cz + Dz));
        }
      }
    }
  }
}

// -1 when a local coordinate sits on the low face, +1 on the high face, 0
// otherwise: the direction in which an edit at it reaches the next chunk.
static int BorderStep(int Local) {
  if (Local == 0) { return -1; }
  if (Local == CHUNK_SIZE - 1) { return 1; }
  return 0;
}

// Returns true when a chunk that should go had to stay because work was still
// in flight on it. The caller must then come back: until it does, that chunk
// stays loaded outside the volume, and a border chunk meshed while it exists
// keeps faces it would not have against an empty neighbour.
static bool EvictUnneededChunks(World *WorldVal, const bool *KeepChunk){
  bool Deferred = false;
  for(int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++){
    if(!WorldVal->SlotInUse[IdxI] || KeepChunk[IdxI]){
      continue;
    }
    if(WorldVal->Chunks[IdxI].IsGenerating || WorldVal->Chunks[IdxI].IsMeshing){
      Deferred = true;
      continue;
    }

    int OldX = WorldVal->Chunks[IdxI].ChunkX;
    int OldY = WorldVal->Chunks[IdxI].ChunkY;
    int OldZ = WorldVal->Chunks[IdxI].ChunkZ;

    if(WorldVal->Chunks[IdxI].IsModified){
      SaveChunkToDisk(&WorldVal->Chunks[IdxI]);
    }

    Chunk *Evicted = &WorldVal->Chunks[IdxI];

    // Still counted as pending: its completion is waiting in the drain (or, if
    // it never generated, it never will). Clearing the flag also turns that
    // waiting completion entry into a stale one the drain skips.
    if (!Evicted->IsGenerated || Evicted->TerrainJustGenerated) {
      WorldVal->PendingGeneration--;
      Evicted->TerrainJustGenerated = false;
    }
    WorldRemoveMeshCandidate(WorldVal, IdxI);
    if(WorldVal->OnChunkEvict != (ChunkEvictCallback)0){
      WorldVal->OnChunkEvict(Evicted);
    }

    RemoveChunkFromMap(WorldVal, OldX, OldY, OldZ);
    UpdateNeighborsDirtyFlag(WorldVal, OldX, OldY, OldZ);
    ChunkResetStorage(Evicted, BLOCK_ID_AIR);

    WorldVal->SlotInUse[IdxI] = false;
    WorldVal->FreeList[WorldVal->FreeCount++] = IdxI;
  }
  return Deferred;
}

static bool CreateOrRecycleChunk(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ){
  if(WorldVal->FreeCount == 0){
    return false;
  }

  int IdxI = WorldVal->FreeList[--WorldVal->FreeCount];

  // A slot above the high-water mark has never held a chunk; its memory is
  // whatever malloc returned, so it starts from a known all-false state.
  if(IdxI >= WorldVal->ChunkCount){
    memset(&WorldVal->Chunks[IdxI], 0, sizeof(WorldVal->Chunks[IdxI]));
  }
  WorldVal->SlotInUse[IdxI] = true;

  // Eviction already returned any buffer; a fresh slot has none to return.
  WorldVal->Chunks[IdxI].Pool = &WorldVal->DensePool;
  WorldVal->Chunks[IdxI].Blocks = (DenseBlockBuffer *)0;
  WorldVal->Chunks[IdxI].Packed = (PackedBlockBuffer *)0;
  WorldVal->Chunks[IdxI].UniformBlockId = BLOCK_ID_AIR;
  WorldVal->Chunks[IdxI].IsUniform = true;

  WorldVal->Chunks[IdxI].ChunkX = ChunkX;
  WorldVal->Chunks[IdxI].ChunkY = ChunkY;
  WorldVal->Chunks[IdxI].ChunkZ = ChunkZ;

  WorldVal->PendingGeneration++;
  EnqueueChunkGeneration(&WorldVal->Chunks[IdxI]);
  InsertChunkIntoMap(WorldVal, IdxI);

  if(IdxI >= WorldVal->ChunkCount){
    WorldVal->ChunkCount = IdxI + 1;
  }
  return true;
}

void UpdateWorld(World *WorldVal, Vec3 PlayerPos, int RenderDist){
  PROFILE_BEGIN(UpdateStart);
  RenderDist = ClampRenderDistance(RenderDist);

  int PChunkX = (int)__builtin_floorf(PlayerPos.x / CHUNK_SIZE);
  int PChunkY = (int)__builtin_floorf(PlayerPos.y / CHUNK_SIZE);
  int PChunkZ = (int)__builtin_floorf(PlayerPos.z / CHUNK_SIZE);

  // Gate: the active-chunk set is a pure function of the player's chunk
  // coordinate and the (clamped) render distance. Skip the recompute when
  // neither changed since the last load. Async generation and meshing run
  // outside this function, so in-flight chunks still complete while stationary.
  if(WorldVal->HasLoadedOnce &&
     WorldVal->LastLoadChunkX == PChunkX &&
     WorldVal->LastLoadChunkY == PChunkY &&
     WorldVal->LastLoadChunkZ == PChunkZ &&
     WorldVal->LastLoadRenderDist == RenderDist){
    PROFILE_END(UpdateStart, PROFILE_WORLD_UPDATE);
    return;
  }

  RebucketMeshCandidates(WorldVal, PChunkX, PChunkY, PChunkZ);

  PROFILE_BEGIN(BoundsStart);
  (void)ColumnBoundsEnsure(&WorldVal->ColumnBounds, PChunkX, PChunkZ,
                           RenderDist, GetWorldSeed());
  PROFILE_END(BoundsStart, PROFILE_COLUMN_BOUNDS);

  // Static: at MAX_ACTIVE_CHUNKS entries it is too large to want on the stack,
  // and UpdateWorld only runs on the thread driving the world.
  static bool KeepChunk[MAX_ACTIVE_CHUNKS];
  memset(KeepChunk, 0, sizeof(KeepChunk));

  const LoadVolume *Volume = GetLoadVolume(WorldVal, PlayerPos, RenderDist);

  MarkUsefulChunks(WorldVal, Volume, KeepChunk);
  bool EvictionDeferred = EvictUnneededChunks(WorldVal, KeepChunk);

  // Volume order is nearest first, so generation is enqueued nearest first.
  bool AllCreated = true;
  for(int Col = 0; Col < Volume->ColumnCount; Col++){
    int ChunkX = Volume->CenterX + Volume->Columns[Col].Dx;
    int ChunkZ = Volume->CenterZ + Volume->Columns[Col].Dz;
    int Layers[MAX_COLUMN_LAYERS];
    int LayerCount = ColumnLayers(Volume, Col, Layers);
    for(int Layer = 0; Layer < LayerCount; Layer++){
      int ChunkY = Layers[Layer];
      if(GetChunkFromWorld(WorldVal, ChunkX, ChunkY, ChunkZ) == (Chunk *)0 &&
         !CreateOrRecycleChunk(WorldVal, ChunkX, ChunkY, ChunkZ)){
        AllCreated = false;
      }
    }
  }

  WorldVal->LastLoadChunkX = PChunkX;
  WorldVal->LastLoadChunkY = PChunkY;
  WorldVal->LastLoadChunkZ = PChunkZ;
  WorldVal->LastLoadRenderDist = RenderDist;
  WorldVal->LastLoadAllCreated = AllCreated;
  // The gate above only opens again when the player changes chunk, so a load
  // that left chunks uncreated or unevicted keeps it shut off until it is done.
  WorldVal->HasLoadedOnce = AllCreated && !EvictionDeferred;
  PROFILE_END(UpdateStart, PROFILE_WORLD_UPDATE);
}

static Chunk* GetLocalCoords(World *WorldVal, Vec3 GlobalPos, int *LocalX, int *LocalY, int *LocalZ){
  int ChunkX = (int)__builtin_floorf(GlobalPos.x / CHUNK_SIZE);
  int ChunkY = (int)__builtin_floorf(GlobalPos.y / CHUNK_SIZE);
  int ChunkZ = (int)__builtin_floorf(GlobalPos.z / CHUNK_SIZE);

  *LocalX = (int)__builtin_floorf(GlobalPos.x) - (ChunkX * CHUNK_SIZE);
  *LocalY = (int)__builtin_floorf(GlobalPos.y) - (ChunkY * CHUNK_SIZE);
  *LocalZ = (int)__builtin_floorf(GlobalPos.z) - (ChunkZ * CHUNK_SIZE);

  return GetChunkFromWorld(WorldVal, ChunkX, ChunkY, ChunkZ);
}

// An edit above everything the terrain bounds allow for its column (a tower,
// a pillar) is recorded so the column's load range keeps reaching it from
// afar. Edits within the bounds are already covered and leave the map alone,
// which keeps load volumes cached. A recorded raise reopens the load gate: the
// volume grew while the player stood still.
static void RaiseEditedColumn(World *WorldVal, const Chunk *ChunkVal) {
  if (WorldVal->ColumnBounds.LayerKey != 0U &&
      ChunkVal->ChunkY <= ColumnBoundsTopLayer(&WorldVal->ColumnBounds,
                                               ChunkVal->ChunkX,
                                               ChunkVal->ChunkZ)) {
    return;
  }
  if (RaisedTopsRaise(&WorldVal->RaisedTops, ChunkVal->ChunkX,
                      ChunkVal->ChunkZ, ChunkVal->ChunkY)) {
    WorldVal->HasLoadedOnce = false;
  }
}

void SetBlockInWorld(World *WorldVal, Vec3 Pos, BlockID BlockId){
  Chunk *ChunkVal = GetChunkAtPos(WorldVal, Pos);
  // A worker owns the storage of a chunk until it is generated.
  if (ChunkVal == (Chunk *)0 || !ChunkVal->IsGenerated) { return; }

  int LocalX = ((int)__builtin_floorf(Pos.x)) % CHUNK_SIZE;
  int LocalY = ((int)__builtin_floorf(Pos.y)) % CHUNK_SIZE;
  int LocalZ = ((int)__builtin_floorf(Pos.z)) % CHUNK_SIZE;

  if (LocalX < 0) { LocalX += CHUNK_SIZE; }
  if (LocalY < 0) { LocalY += CHUNK_SIZE; }
  if (LocalZ < 0) { LocalZ += CHUNK_SIZE; }

  BlockID PreviousId = ChunkGetLocal(ChunkVal, LocalX, LocalY, LocalZ);
  SetBlockInChunk(ChunkVal, (Vec3){(float)LocalX, (float)LocalY, (float)LocalZ}, BlockId);
  MarkChunkDirty(WorldVal, ChunkVal);
  RaiseEditedColumn(WorldVal, ChunkVal);

  // A block on a face, edge or corner of its chunk is read by the face, edge
  // and corner neighbours across it: face culling by the first, ambient
  // occlusion by all of them. On each axis the offset is 0 or the one step
  // toward the border the block touches.
  int StepX = BorderStep(LocalX);
  int StepY = BorderStep(LocalY);
  int StepZ = BorderStep(LocalZ);
  for (int Dx = -1; Dx <= 1; Dx++) {
    if (Dx != 0 && Dx != StepX) { continue; }
    for (int Dy = -1; Dy <= 1; Dy++) {
      if (Dy != 0 && Dy != StepY) { continue; }
      for (int Dz = -1; Dz <= 1; Dz++) {
        if ((Dz != 0 && Dz != StepZ) || (Dx == 0 && Dy == 0 && Dz == 0)) {
          continue;
        }
        MarkChunkDirty(WorldVal, GetChunkFromWorld(WorldVal, ChunkVal->ChunkX + Dx,
                                         ChunkVal->ChunkY + Dy,
                                         ChunkVal->ChunkZ + Dz));
      }
    }
  }

  if (WorldVal->OnBlockChange != (BlockChangeCallback)0 && PreviousId != BlockId) {
    WorldVal->OnBlockChange(WorldVal->BlockChangeUserData,
                            (int)__builtin_floorf(Pos.x),
                            (int)__builtin_floorf(Pos.y),
                            (int)__builtin_floorf(Pos.z), BlockId);
  }
}

int GetBlockIDFromWorld(World *WorldVal, Vec3 GlobalPos){
  int Lx; 
  int Ly; 
  int Lz;

  Chunk *ChunkVal = GetLocalCoords(WorldVal, GlobalPos, &Lx, &Ly, &Lz);

  // Not generated reads as unloaded: the worker may still swap its storage.
  if(ChunkVal != (Chunk *)0 && ChunkVal->IsGenerated){
    return GetBlockIdInChunk(ChunkVal, (Vec3){(float)Lx, (float)Ly, (float)Lz});
  }

  return 0;
}

// Floor division, so a negative cell index maps to the chunk below rather than
// truncating toward zero and landing in the wrong one.
static int FloorDivCells(int Value, int Divisor) {
  int Quotient = Value / Divisor;
  if ((Value % Divisor != 0) && ((Value < 0) != (Divisor < 0))) {
    Quotient--;
  }
  return Quotient;
}

unsigned char GetBiomeCellFromWorld(World *WorldVal, int CellX, int CellY,
                                    int CellZ, unsigned char Fallback) {
  int ChunkX = FloorDivCells(CellX, BIOME_CELLS_PER_CHUNK);
  int ChunkY = FloorDivCells(CellY, BIOME_CELLS_PER_CHUNK);
  int ChunkZ = FloorDivCells(CellZ, BIOME_CELLS_PER_CHUNK);

  Chunk *ChunkVal = GetChunkFromWorld(WorldVal, ChunkX, ChunkY, ChunkZ);
  if (ChunkVal == (Chunk *)0) {
    return Fallback;
  }

  int LocalX = CellX - (ChunkX * BIOME_CELLS_PER_CHUNK);
  int LocalY = CellY - (ChunkY * BIOME_CELLS_PER_CHUNK);
  int LocalZ = CellZ - (ChunkZ * BIOME_CELLS_PER_CHUNK);

  return ChunkVal->BiomeMap[LocalX][LocalY][LocalZ];
}

static DdaState InitDDAState(Vec3 RayOrigin, Vec3 RayDir){
  DdaState State = {0};

  float StartX = RayOrigin.x + BLOCK_HALF_SIZE;
  float StartY = RayOrigin.y + BLOCK_HALF_SIZE;
  float StartZ = RayOrigin.z + BLOCK_HALF_SIZE;

  State.VoxelX = (int)__builtin_floorf(StartX);
  State.VoxelY = (int)__builtin_floorf(StartY);
  State.VoxelZ = (int)__builtin_floorf(StartZ);

  State.StepX = (RayDir.x > 0.0F) - (RayDir.x < 0.0F);
  State.StepY = (RayDir.y > 0.0F) - (RayDir.y < 0.0F);
  State.StepZ = (RayDir.z > 0.0F) - (RayDir.z < 0.0F);

  State.TDeltaX = (RayDir.x != 0.0F) ? __builtin_fabsf(BLOCK_SIZE / RayDir.x) : __builtin_huge_valf();
  State.TDeltaY = (RayDir.y != 0.0F) ? __builtin_fabsf(BLOCK_SIZE / RayDir.y) : __builtin_huge_valf();
  State.TDeltaZ = (RayDir.z != 0.0F) ? __builtin_fabsf(BLOCK_SIZE / RayDir.z) : __builtin_huge_valf();

  State.TMaxX = (State.StepX > 0) ? (__builtin_floorf(StartX) + BLOCK_SIZE - StartX) * State.TDeltaX : (StartX - __builtin_floorf(StartX)) * State.TDeltaX;
  State.TMaxY = (State.StepY > 0) ? (__builtin_floorf(StartY) + BLOCK_SIZE - StartY) * State.TDeltaY : (StartY - __builtin_floorf(StartY)) * State.TDeltaY;
  State.TMaxZ = (State.StepZ > 0) ? (__builtin_floorf(StartZ) + BLOCK_SIZE - StartZ) * State.TDeltaZ : (StartZ - __builtin_floorf(StartZ)) * State.TDeltaZ;

  return State;
}

static void StepDDA(DdaState *State){
  if(State->TMaxX < State->TMaxY){
    if(State->TMaxX < State->TMaxZ){
      State->VoxelX += State->StepX;
      State->DistanceTravelled = State->TMaxX;
      State->TMaxX += State->TDeltaX;
      State->LastStep = 0;
    } else {
      State->VoxelZ += State->StepZ;
      State->DistanceTravelled = State->TMaxZ;
      State->TMaxZ += State->TDeltaZ;
      State->LastStep = 2;
    }
  } else {
    if(State->TMaxY < State->TMaxZ){
      State->VoxelY += State->StepY;
      State->DistanceTravelled = State->TMaxY;
      State->TMaxY += State->TDeltaY;
      State->LastStep = 1;
    } else {
      State->VoxelZ += State->StepZ;
      State->DistanceTravelled = State->TMaxZ;
      State->TMaxZ += State->TDeltaZ;
      State->LastStep = 2;
    }
  }
}

RaycastResult RayCastToWorld(World *WorldVal, Vec3 RayOrigin, Vec3 RayDir, float MaxDistance){
  RaycastResult Result = { .Hit = false, .BlockPos ={0.0F, 0.0F, 0.0F}, .BlockId = 0, .Normal = {0.0F, 0.0F, 0.0F} };

  DdaState Dda = InitDDAState(RayOrigin, RayDir);

  int MaxIter = (int)(MaxDistance * DDA_MAX_CROSSED_AXES) + 1; 

  for(int IdxI = 0; IdxI < MaxIter; IdxI++){
    Vec3 CheckPos = { (float)Dda.VoxelX, (float)Dda.VoxelY, (float)Dda.VoxelZ };
    int Id = GetBlockIDFromWorld(WorldVal, CheckPos);

    if(Id != 0){
      Result.Hit = true;
      Result.BlockPos = CheckPos;
      Result.BlockId = Id;

      if(Dda.LastStep == 0) { Result.Normal = (Vec3){ (float)-Dda.StepX, 0.0F, 0.0F }; }
      else if(Dda.LastStep == 1) { Result.Normal = (Vec3){ 0.0F, (float)-Dda.StepY, 0.0F }; }
      else if(Dda.LastStep == 2) { Result.Normal = (Vec3){ 0.0F, 0.0F, (float)-Dda.StepZ }; }

      return Result;
    }

    StepDDA(&Dda);

    if(Dda.DistanceTravelled > MaxDistance) { break; }
  }

  return Result;
}

// Covers the same 26 chunks the mesh snapshot reads. A loaded neighbour still
// generating would be captured as air, and its completion would then have to
// remesh this chunk; waiting for it instead meshes each chunk once. An absent
// neighbour lies outside the load volume and counts as air for good.
bool AreNeighborsGenerated(World *WorldVal, Chunk *ChunkVal) {
  for (int Dx = -1; Dx <= 1; Dx++) {
    for (int Dy = -1; Dy <= 1; Dy++) {
      for (int Dz = -1; Dz <= 1; Dz++) {
        if (Dx == 0 && Dy == 0 && Dz == 0) {
          continue;
        }
        const Chunk *Neighbor =
            GetChunkFromWorld(WorldVal, ChunkVal->ChunkX + Dx,
                              ChunkVal->ChunkY + Dy, ChunkVal->ChunkZ + Dz);
        if (Neighbor != (Chunk *)0 &&
            (!Neighbor->IsGenerated || Neighbor->IsGenerating)) {
          return false;
        }
      }
    }
  }
  return true;
}

void WorldGetLoadLayerRange(World *WorldVal, Vec3 Center, int RenderDist,
                            LoadLayerRange *Out) {
  const LoadVolume *Volume = GetLoadVolume(WorldVal, Center, RenderDist);
  Out->Lo = Volume->WindowLo;
  Out->Hi = Volume->WindowHi;
  Out->Count = Volume->LayerCount;
}

void WorldGetLoadVolumeStats(World *WorldVal, Vec3 Center, int RenderDist,
                             LoadVolumeStats *Out) {
  const LoadVolume *Volume = GetLoadVolume(WorldVal, Center, RenderDist);
  LoadVolumeStats Stats = {0};
  Stats.Columns = Volume->ColumnCount;
  Stats.NearColumns = Volume->NearCount;
  Stats.Chunks = Volume->ChunkTotal;
  Stats.TrimmedColumns = Volume->TrimmedColumns;
  Stats.RaisedColumns = WorldVal->RaisedTops.Count;
  for (int Col = 0; Col < Volume->ColumnCount; Col++) {
    if (Volume->BandHi[Col] < Volume->BandLo[Col]) { continue; }
    int Hi = Volume->BandHi[Col];
    if (Stats.BandColumns == 0 || Hi > Stats.MaxLayer) { Stats.MaxLayer = Hi; }
    Stats.BandColumns++;
  }
  *Out = Stats;
}

// Above the generation ceiling nothing is ever generated, so an absent chunk
// there is air even below a column's highest loaded layer (the window keeps
// the centre's neighbours when the centre is above the world).
bool WorldIsUnloadedRock(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ) {
  int Top = 0;
  return WorldGetLoadedColumnTop(WorldVal, ChunkX, ChunkZ, &Top) &&
         ChunkY <= Top && ChunkY <= WorldVal->WorldHiChunkY;
}

unsigned WorldUnloadedRockFaces(World *WorldVal, int ChunkX, int ChunkY,
                                int ChunkZ) {
  enum { FACE_NEIGHBOURS = 6 };
  static const int STEP[FACE_NEIGHBOURS][3] = {{-1, 0, 0}, {1, 0, 0},
                                               {0, -1, 0}, {0, 1, 0},
                                               {0, 0, -1}, {0, 0, 1}};
  unsigned Faces = 0;
  for (int Face = 0; Face < FACE_NEIGHBOURS; Face++) {
    int X = ChunkX + STEP[Face][0];
    int Y = ChunkY + STEP[Face][1];
    int Z = ChunkZ + STEP[Face][2];
    if (GetChunkFromWorld(WorldVal, X, Y, Z) == (Chunk *)0 &&
        WorldIsUnloadedRock(WorldVal, X, Y, Z)) {
      Faces |= 1U << (unsigned)Face;
    }
  }
  return Faces;
}

bool WorldIsUnloadedSky(World *WorldVal, int ChunkX, int ChunkY, int ChunkZ) {
  int Top = 0;
  return WorldGetLoadedColumnTop(WorldVal, ChunkX, ChunkZ, &Top) &&
         (ChunkY > Top || ChunkY > WorldVal->WorldHiChunkY);
}

void WorldGetColumnBoundsStats(World *WorldVal, Vec3 Center, int RenderDist,
                               ColumnBoundsStats *Out) {
  const LoadVolume *Volume = GetLoadVolume(WorldVal, Center, RenderDist);
  LoadLayerRange Window = {Volume->WindowLo, Volume->WindowHi,
                           Volume->LayerCount};
  int Span = Window.Hi - Window.Lo + 1;
  ColumnBoundsTable *Table = &WorldVal->ColumnBounds;

  ColumnBoundsStats Stats = {0};
  for (int Col = 0; Col < Volume->ColumnCount; Col++) {
    int ColumnX = Volume->CenterX + Volume->Columns[Col].Dx;
    int ColumnZ = Volume->CenterZ + Volume->Columns[Col].Dz;
    int Top = ColumnBoundsTopLayer(Table, ColumnX, ColumnZ);
    int Bottom = ColumnBoundsBottomLayer(Table, ColumnX, ColumnZ);
    if (Top > WorldVal->WorldHiChunkY) { Top = WorldVal->WorldHiChunkY; }
    if (Bottom < WorldVal->WorldLoChunkY) { Bottom = WorldVal->WorldLoChunkY; }
    if (Bottom > Top) { Bottom = Top; }

    if (Stats.Columns == 0 || Top > Stats.MaxTopLayer) { Stats.MaxTopLayer = Top; }
    if (Stats.Columns == 0 || Bottom < Stats.MinBottomLayer) {
      Stats.MinBottomLayer = Bottom;
    }
    Stats.Columns++;
    Stats.WindowChunks += Span;
    Stats.BandChunks += Top - Bottom + 1;
    if (Top > Window.Hi) {
      Stats.SlicedColumns++;
      Stats.SlicedLayers += Top - Window.Hi;
    }
    if (Bottom < Window.Lo) { Stats.UncoveredBelowColumns++; }
    if (Window.Hi > Top) {
      int AirFrom = (Top >= Window.Lo) ? Top + 1 : Window.Lo;
      Stats.AirWindowChunks += Window.Hi - AirFrom + 1;
    }
  }
  Stats.ReachColumns = Table->ReachColumns;
  Stats.ReachAbove = Table->ReachAbove;
  Stats.Computed = Table->Computed;
  *Out = Stats;
}

// Neighbour-dirty reaction to one finished chunk, shared by the drain and its
// overflow fallback.
static void HandleGenerationEvent(World *WorldVal, Chunk *ChunkVal) {
  UpdateNeighborsDirtyFlag(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY,
                           ChunkVal->ChunkZ);
  ChunkVal->TerrainJustGenerated = false;
  WorldVal->PendingGeneration--;
  // Generation left the chunk itself dirty, and it was not a candidate while
  // generating.
  AddMeshCandidate(WorldVal, ChunkVal);
}

void WorldProcessGenerationEvents(World *WorldVal) {
  // Static: sized for every active chunk, far too large for the stack, and only
  // the thread driving the world calls this.
  static Chunk *Drained[MAX_ACTIVE_CHUNKS];
  bool Overflowed = false;
  int Count = DrainChunkCompletions(Drained, MAX_ACTIVE_CHUNKS, &Overflowed);

  // An entry outlives nothing it refers to: its slot may have been evicted, or
  // reused and not yet regenerated, or already handled through a duplicate
  // entry. The flags tell all three apart from a live completion.
  for (int Idx = 0; Idx < Count; Idx++) {
    Chunk *ChunkVal = Drained[Idx];
    if (WorldVal->SlotInUse[ChunkVal - WorldVal->Chunks] &&
        ChunkVal->IsGenerated && ChunkVal->TerrainJustGenerated) {
      HandleGenerationEvent(WorldVal, ChunkVal);
    }
  }

  if (Overflowed) {
    for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
      Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
      if (WorldVal->SlotInUse[IdxI] && ChunkVal->IsGenerated &&
          ChunkVal->TerrainJustGenerated) {
        HandleGenerationEvent(WorldVal, ChunkVal);
      }
    }
  }
}

// Whether the last load request was made for the volume a caller asks about.
// The counters describe that request only: right after a teleport, before
// UpdateWorld has run, they still describe the old position.
static bool LastLoadMatches(const World *WorldVal, Vec3 Center, int RenderDist) {
  return WorldVal->LastLoadChunkX == (int)__builtin_floorf(Center.x / CHUNK_SIZE) &&
         WorldVal->LastLoadChunkY == (int)__builtin_floorf(Center.y / CHUNK_SIZE) &&
         WorldVal->LastLoadChunkZ == (int)__builtin_floorf(Center.z / CHUNK_SIZE) &&
         WorldVal->LastLoadRenderDist == ClampRenderDistance(RenderDist);
}

#ifdef MINEGAME_PROFILE
// The volume walk the counters replaced, kept to verify them.
static bool WalkGenerationSettled(World *WorldVal, Vec3 Center, int RenderDist) {
  const LoadVolume *Volume = GetLoadVolume(WorldVal, Center, RenderDist);

  for (int Col = 0; Col < Volume->ColumnCount; Col++) {
    int ChunkX = Volume->CenterX + Volume->Columns[Col].Dx;
    int ChunkZ = Volume->CenterZ + Volume->Columns[Col].Dz;
    int Layers[MAX_COLUMN_LAYERS];
    int LayerCount = ColumnLayers(Volume, Col, Layers);
    for (int Layer = 0; Layer < LayerCount; Layer++) {
      Chunk *ChunkVal =
          GetChunkFromWorld(WorldVal, ChunkX, Layers[Layer], ChunkZ);
      if (ChunkVal == (Chunk *)0) {
        return false;
      }
      if (!ChunkVal->IsGenerated || ChunkVal->IsGenerating ||
          ChunkVal->TerrainJustGenerated) {
        return false;
      }
    }
  }
  return true;
}
#endif

// The counter counts every chunk not yet through the drain, inside the volume or
// left outside it by a deferred eviction, so this can only be stricter than the
// volume walk it replaced, never looser.
bool WorldIsGenerationSettled(World *WorldVal, Vec3 Center, int RenderDist) {
  bool Settled = LastLoadMatches(WorldVal, Center, RenderDist) &&
                 WorldVal->LastLoadAllCreated &&
                 WorldVal->PendingGeneration == 0;

#ifdef MINEGAME_PROFILE
  if (Settled && !WorldVal->LastGenerationSettled) {
    if (WalkGenerationSettled(WorldVal, Center, RenderDist)) {
      LogInfo("WORLD: generation settle counters agree with the volume walk");
    } else {
      LogError("WORLD: generation settle counters report settled, the volume "
               "walk does not");
    }
  }
  WorldVal->LastGenerationSettled = Settled;
#endif
  return Settled;
}

bool WorldIsSettled(World *WorldVal, Vec3 Center, int RenderDist) {
  // A load still owes evictions while this is false. A chunk left outside the
  // volume is still drawn and still a neighbour to the border chunks, so the
  // world is not in the state it is converging to until it has gone.
  if (!WorldVal->HasLoadedOnce) {
    return false;
  }
  if (!WorldIsGenerationSettled(WorldVal, Center, RenderDist)) {
    return false;
  }

  // Every generated chunk that is dirty or meshing is a candidate, and with the
  // load complete and no eviction deferred the slots in use are exactly the
  // volume. Membership drops lazily, so this reads settled one queueing pass
  // after the last chunk went clean.
  bool Settled = WorldVal->CandidateCount == 0;

#ifdef MINEGAME_PROFILE
  if (Settled && !WorldVal->LastSettled) {
    int Missed = 0;
    for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
      const Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
      if (WorldVal->SlotInUse[IdxI] &&
          (ChunkVal->IsDirty || ChunkVal->IsMeshing)) {
        Missed++;
      }
    }
    if (Missed == 0) {
      LogInfo("WORLD: mesh candidate set agrees with the slot walk");
    } else {
      LogError("WORLD: %d dirty or meshing chunks are missing from the mesh "
               "candidate set",
               Missed);
    }
  }
  WorldVal->LastSettled = Settled;
#endif
  return Settled;
}

// Present, generated, and neither dirty nor meshing.
static bool ChunkIsSettled(const Chunk *ChunkVal) {
  return ChunkVal != (Chunk *)0 && ChunkVal->IsGenerated &&
         !ChunkVal->IsGenerating && !ChunkVal->TerrainJustGenerated &&
         !ChunkVal->IsDirty && !ChunkVal->IsMeshing;
}

bool WorldIsNearSettled(World *WorldVal, Vec3 Center, int RenderDist,
                        int Radius) {
  // Until the load for this centre has been requested, the chunks around it
  // are the old position's, or absent.
  if (!LastLoadMatches(WorldVal, Center, RenderDist)) {
    return false;
  }

  const LoadVolume *Volume = GetLoadVolume(WorldVal, Center, RenderDist);
  int RadiusSq = Radius * Radius;

  // Columns are sorted nearest first, so the first one beyond Radius ends it.
  for (int Col = 0; Col < Volume->ColumnCount; Col++) {
    int Dx = Volume->Columns[Col].Dx;
    int Dz = Volume->Columns[Col].Dz;
    if ((Dx * Dx) + (Dz * Dz) > RadiusSq) {
      break;
    }
    int Layers[MAX_COLUMN_LAYERS];
    int LayerCount = ColumnLayers(Volume, Col, Layers);
    for (int Layer = 0; Layer < LayerCount; Layer++) {
      if (!ChunkIsSettled(GetChunkFromWorld(WorldVal, Volume->CenterX + Dx,
                                            Layers[Layer],
                                            Volume->CenterZ + Dz))) {
        return false;
      }
    }
  }
  return true;
}

bool WorldIsSurfaceSettled(World *WorldVal, Vec3 Center, int RenderDist) {
  if (!LastLoadMatches(WorldVal, Center, RenderDist)) {
    return false;
  }

  const LoadVolume *Volume = GetLoadVolume(WorldVal, Center, RenderDist);
  for (int Col = 0; Col < Volume->ColumnCount; Col++) {
    int ChunkX = Volume->CenterX + Volume->Columns[Col].Dx;
    int ChunkZ = Volume->CenterZ + Volume->Columns[Col].Dz;
    int Hi = Volume->BandHi[Col];
    int Lo = Hi - WORLD_SURFACE_SETTLE_LAYERS + 1;
    if (Lo < Volume->BandLo[Col]) { Lo = Volume->BandLo[Col]; }
    for (int Y = Hi; Y >= Lo; Y--) {
      if (!ChunkIsSettled(GetChunkFromWorld(WorldVal, ChunkX, Y, ChunkZ))) {
        return false;
      }
    }
  }
  return true;
}

void WorldGetLoadCounts(World *WorldVal, int *OutGenerated, int *OutGenerating,
                        int *OutDirty, int *OutMeshing) {
  int Generated = 0;
  int Generating = 0;
  int Dirty = 0;
  int Meshing = 0;

  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    if (!WorldVal->SlotInUse[IdxI]) {
      continue;
    }
    Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
    if (ChunkVal->IsGenerated) {
      Generated++;
    }
    if (ChunkVal->IsGenerating) {
      Generating++;
    }
    if (ChunkVal->IsDirty) {
      Dirty++;
    }
    if (ChunkVal->IsMeshing) {
      Meshing++;
    }
  }

  if (OutGenerated != (int *)0) { *OutGenerated = Generated; }
  if (OutGenerating != (int *)0) { *OutGenerating = Generating; }
  if (OutDirty != (int *)0) { *OutDirty = Dirty; }
  if (OutMeshing != (int *)0) { *OutMeshing = Meshing; }
}
