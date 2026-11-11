#include "world/stone_filler.h"
#include "core/fileio.h"
#include "core/hash.h"
#include "core/log.h"
#include "third_party/cJSON.h"
#include "world/block_system.h"
#include <stdlib.h>

typedef struct {
  BlockID Block;
  // The layer starts below Boundary; the band spans Boundary +- Band / 2. The
  // first layer has no boundary.
  int Boundary;
  int Band;
} FillerLayer;

typedef struct {
  BlockID Block;
  int MinY;
  int MaxY;
  float Chance;
  int RadiusMin;
  int RadiusMax;
} PocketKind;

typedef struct {
  FillerLayer Layers[FILLER_MAX_LAYERS];
  int LayerCount;
  PocketKind Kinds[FILLER_MAX_POCKET_KINDS];
  int KindCount;
  int PocketCell;
} FillerState;

#define FILLER_BAND_SALT 0xF111E40000000001ULL
#define FILLER_POCKET_SALT 0xF111E40000000002ULL

static FillerState *GetFillerState(void) {
  static FillerState SState;
  return &SState;
}

static void InstallStoneOnly(FillerState *State) {
  const int DEFAULT_POCKET_CELL = 32;
  int Stone = GetBlockIdByName("Stone");
  State->Layers[0] = (FillerLayer){
      .Block = (BlockID)((Stone > 0) ? Stone : 0), .Boundary = 0, .Band = 0};
  State->LayerCount = 1;
  State->KindCount = 0;
  State->PocketCell = DEFAULT_POCKET_CELL;
}

void InitStoneFiller(void) { InstallStoneOnly(GetFillerState()); }

static bool ResolveBlock(const cJSON *Item, const char *FilePath,
                         BlockID *Out) {
  const cJSON *Name = cJSON_GetObjectItemCaseSensitive(Item, "block");
  int Id = cJSON_IsString(Name) ? GetBlockIdByName(Name->valuestring) : -1;
  if (Id <= 0) {
    LogError("FILLER: %s names a block that is not loaded", FilePath);
    return false;
  }
  *Out = (BlockID)Id;
  return true;
}

static int ReadIntOr(const cJSON *Item, const char *Key, int Fallback) {
  const cJSON *Value = cJSON_GetObjectItemCaseSensitive(Item, Key);
  return cJSON_IsNumber(Value) ? Value->valueint : Fallback;
}

static bool ParseLayers(const cJSON *Json, const char *FilePath,
                        FillerState *Out) {
  const cJSON *Layers = cJSON_GetObjectItemCaseSensitive(Json, "layers");
  int Count = cJSON_IsArray(Layers) ? cJSON_GetArraySize(Layers) : 0;
  if (Count < 1 || Count > FILLER_MAX_LAYERS) {
    LogError("FILLER: %s needs 1..%d layers", FilePath, FILLER_MAX_LAYERS);
    return false;
  }
  for (int Idx = 0; Idx < Count; Idx++) {
    const cJSON *Item = cJSON_GetArrayItem(Layers, Idx);
    FillerLayer *Layer = &Out->Layers[Idx];
    if (!ResolveBlock(Item, FilePath, &Layer->Block)) {
      return false;
    }
    Layer->Boundary = ReadIntOr(Item, "below", 0);
    Layer->Band = ReadIntOr(Item, "band", 0);
    bool Descending =
        Idx < 2 || Layer->Boundary < Out->Layers[Idx - 1].Boundary;
    if (Idx > 0 && (Layer->Band < 0 || !Descending)) {
      LogError("FILLER: %s layer %d needs a non-negative band and a boundary "
               "below the previous layer's",
               FilePath, Idx);
      return false;
    }
  }
  Out->LayerCount = Count;
  return true;
}

static bool ParsePockets(const cJSON *Json, const char *FilePath,
                         FillerState *Out) {
  Out->PocketCell = ReadIntOr(Json, "pocketCell", Out->PocketCell);
  const cJSON *Pockets = cJSON_GetObjectItemCaseSensitive(Json, "pockets");
  int Count = cJSON_IsArray(Pockets) ? cJSON_GetArraySize(Pockets) : 0;
  if (Count > FILLER_MAX_POCKET_KINDS || Out->PocketCell < 2) {
    LogError("FILLER: %s needs at most %d pocket kinds and a pocketCell of 2 "
             "or more",
             FilePath, FILLER_MAX_POCKET_KINDS);
    return false;
  }
  for (int Idx = 0; Idx < Count; Idx++) {
    const cJSON *Item = cJSON_GetArrayItem(Pockets, Idx);
    PocketKind *Kind = &Out->Kinds[Idx];
    if (!ResolveBlock(Item, FilePath, &Kind->Block)) {
      return false;
    }
    Kind->MinY = ReadIntOr(Item, "minY", 0);
    Kind->MaxY = ReadIntOr(Item, "maxY", 0);
    const cJSON *Chance = cJSON_GetObjectItemCaseSensitive(Item, "chance");
    Kind->Chance = cJSON_IsNumber(Chance) ? (float)Chance->valuedouble : 0.0F;
    const cJSON *Radius = cJSON_GetObjectItemCaseSensitive(Item, "radius");
    Kind->RadiusMin = cJSON_GetArrayItem(Radius, 0) != NULL
                          ? cJSON_GetArrayItem(Radius, 0)->valueint
                          : 0;
    Kind->RadiusMax = cJSON_GetArrayItem(Radius, 1) != NULL
                          ? cJSON_GetArrayItem(Radius, 1)->valueint
                          : 0;
    // A pocket may reach only its own cell's neighbours, which is what lets a
    // cube look at the 3 x 3 x 3 cells around it and nothing more.
    if (Kind->RadiusMin < 1 || Kind->RadiusMax < Kind->RadiusMin ||
        Kind->RadiusMax * 2 > Out->PocketCell || Kind->MaxY < Kind->MinY) {
      LogError("FILLER: %s pocket %d needs 1 <= radius min <= max <= "
               "pocketCell / 2 and minY <= maxY",
               FilePath, Idx);
      return false;
    }
  }
  Out->KindCount = Count;
  return true;
}

void LoadStoneFiller(const char *FilePath) {
  FillerState *State = GetFillerState();
  InstallStoneOnly(State);
  char *Content = ReadTextFile(FilePath);
  cJSON *Json = (Content != NULL) ? cJSON_Parse(Content) : NULL;
  free(Content);
  if (Json == NULL) {
    LogError("FILLER: could not read %s, using stone only", FilePath);
    return;
  }

  FillerState Parsed = *State;
  if (ParseLayers(Json, FilePath, &Parsed) &&
      ParsePockets(Json, FilePath, &Parsed)) {
    *State = Parsed;
    LogInfo("FILLER: %d layers, %d pocket kinds, pocket cell %d",
            State->LayerCount, State->KindCount, State->PocketCell);
  } else {
    LogError("FILLER: %s rejected, using stone only", FilePath);
  }
  cJSON_Delete(Json);
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

static int FloorDiv(int Value, int Divisor) {
  int Quotient = Value / Divisor;
  if ((Value % Divisor != 0) && (Value < 0)) {
    Quotient--;
  }
  return Quotient;
}

static uint64_t HashPosition(int X, int Y, int Z, uint64_t Seed) {
  const uint64_t PRIME_X = 0x9E3779B97F4A7C15ULL;
  const uint64_t PRIME_Y = 0xD6E8FEB86659FD93ULL;
  const uint64_t PRIME_Z = 0xC2B2AE3D27D4EB4FULL;
  uint64_t Hash = SplitMix64(Seed);
  Hash = SplitMix64(Hash ^ ((uint64_t)(uint32_t)X * PRIME_X));
  Hash = SplitMix64(Hash ^ ((uint64_t)(uint32_t)Y * PRIME_Y));
  return SplitMix64(Hash ^ ((uint64_t)(uint32_t)Z * PRIME_Z));
}

static float HashUnit(uint64_t Hash) {
  const uint64_t MASK24 = 0xFFFFFFULL;
  const float SCALE = 16777216.0F;
  return (float)(Hash & MASK24) / SCALE;
}

// The layer index a Y lies fully in, or -(lower layer index) - 1 when Y is in
// the band above that layer.
static int ClassifyRow(const FillerState *State, int Y) {
  int Layer = 0;
  for (int Idx = 1; Idx < State->LayerCount; Idx++) {
    const FillerLayer *Next = &State->Layers[Idx];
    int Half = Next->Band / 2;
    int Top = Next->Boundary + (Next->Band - Half);
    int Bottom = Next->Boundary - Half;
    if (Y >= Top) {
      break;
    }
    if (Y >= Bottom) {
      return -Idx - 1;
    }
    Layer = Idx;
  }
  return Layer;
}

// The pocket of one cell, if the cell has one.
static bool CellPocket(const FillerState *State, int CellX, int CellY, int CellZ,
                       uint64_t Seed, FillerPocket *Out) {
  int Size = State->PocketCell;
  int CenterY = (CellY * Size) + (Size / 2);
  int Eligible[FILLER_MAX_POCKET_KINDS];
  int EligibleCount = 0;
  for (int Idx = 0; Idx < State->KindCount; Idx++) {
    if (CenterY >= State->Kinds[Idx].MinY && CenterY <= State->Kinds[Idx].MaxY) {
      Eligible[EligibleCount++] = Idx;
    }
  }
  if (EligibleCount == 0) {
    return false;
  }

  uint64_t Hash = HashPosition(CellX, CellY, CellZ, Seed ^ FILLER_POCKET_SALT);
  const PocketKind *Kind =
      &State->Kinds[Eligible[SplitMix64(Hash) % (uint64_t)EligibleCount]];
  if (HashUnit(SplitMix64(Hash + 1U)) >= Kind->Chance) {
    return false;
  }
  uint64_t Span = (uint64_t)Kind->RadiusMax - (uint64_t)Kind->RadiusMin + 1U;
  enum { DRAW_CENTER = 2, DRAW_RADIUS = 5 };
  int *Centers[3] = {&Out->CenterX, &Out->CenterY, &Out->CenterZ};
  int *Radii[3] = {&Out->RadiusX, &Out->RadiusY, &Out->RadiusZ};
  int Origins[3] = {CellX * Size, CellY * Size, CellZ * Size};
  for (int Axis = 0; Axis < 3; Axis++) {
    *Centers[Axis] =
        Origins[Axis] +
        (int)(SplitMix64(Hash + DRAW_CENTER + (uint64_t)Axis) % (uint64_t)Size);
    *Radii[Axis] =
        Kind->RadiusMin +
        (int)(SplitMix64(Hash + DRAW_RADIUS + (uint64_t)Axis) % Span);
  }
  Out->MinY = Kind->MinY;
  Out->MaxY = Kind->MaxY;
  Out->Block = Kind->Block;
  return true;
}

static bool PocketReachesCube(const FillerPocket *Pocket, int MinX, int MinY,
                              int MinZ) {
  const int LAST = FILLER_ROWS - 1;
  return Pocket->CenterX + Pocket->RadiusX >= MinX &&
         Pocket->CenterX - Pocket->RadiusX <= MinX + LAST &&
         Pocket->CenterY + Pocket->RadiusY >= MinY &&
         Pocket->CenterY - Pocket->RadiusY <= MinY + LAST &&
         Pocket->CenterZ + Pocket->RadiusZ >= MinZ &&
         Pocket->CenterZ - Pocket->RadiusZ <= MinZ + LAST &&
         Pocket->MaxY >= MinY && Pocket->MinY <= MinY + LAST;
}

void FillerCubeInit(FillerCube *Cube, int ChunkX, int ChunkY, int ChunkZ,
                    uint64_t Seed) {
  const FillerState *State = GetFillerState();
  int MinX = ChunkX * FILLER_ROWS;
  int MinY = ChunkY * FILLER_ROWS;
  int MinZ = ChunkZ * FILLER_ROWS;
  Cube->BaseY = MinY;
  Cube->Seed = Seed;
  for (int Row = 0; Row < FILLER_ROWS; Row++) {
    int Class = ClassifyRow(State, MinY + Row);
    Cube->RowBand[Row] = (Class < 0) ? -Class - 1 : -1;
    Cube->RowBlock[Row] = (Class < 0) ? 0 : State->Layers[Class].Block;
  }

  Cube->PocketCount = 0;
  if (State->KindCount == 0) {
    return;
  }
  int Size = State->PocketCell;
  int Center[3] = {FloorDiv(MinX, Size), FloorDiv(MinY, Size),
                   FloorDiv(MinZ, Size)};
  for (int Dx = -1; Dx <= 1; Dx++) {
    for (int Dy = -1; Dy <= 1; Dy++) {
      for (int Dz = -1; Dz <= 1; Dz++) {
        FillerPocket Pocket;
        if (CellPocket(State, Center[0] + Dx, Center[1] + Dy, Center[2] + Dz,
                       Seed, &Pocket) &&
            PocketReachesCube(&Pocket, MinX, MinY, MinZ)) {
          Cube->Pockets[Cube->PocketCount++] = Pocket;
        }
      }
    }
  }
}

bool FillerCubeUniform(const FillerCube *Cube, BlockID *OutBlock) {
  if (Cube->PocketCount > 0) {
    return false;
  }
  for (int Row = 0; Row < FILLER_ROWS; Row++) {
    if (Cube->RowBlock[Row] == 0 || Cube->RowBlock[Row] != Cube->RowBlock[0]) {
      return false;
    }
  }
  *OutBlock = Cube->RowBlock[0];
  return true;
}

static bool InsidePocket(const FillerPocket *Pocket, int X, int Y, int Z) {
  if (Y < Pocket->MinY || Y > Pocket->MaxY) {
    return false;
  }
  float Dx = (float)(X - Pocket->CenterX) / (float)Pocket->RadiusX;
  float Dy = (float)(Y - Pocket->CenterY) / (float)Pocket->RadiusY;
  float Dz = (float)(Z - Pocket->CenterZ) / (float)Pocket->RadiusZ;
  return (Dx * Dx) + (Dy * Dy) + (Dz * Dz) <= 1.0F;
}

BlockID FillerBlockAt(const FillerCube *Cube, int X, int Y, int Z) {
  for (int Idx = 0; Idx < Cube->PocketCount; Idx++) {
    if (InsidePocket(&Cube->Pockets[Idx], X, Y, Z)) {
      return Cube->Pockets[Idx].Block;
    }
  }
  int Row = Y - Cube->BaseY;
  if (Cube->RowBlock[Row] != 0) {
    return Cube->RowBlock[Row];
  }
  // Transition band: the lower layer becomes likelier toward the band's
  // bottom.
  const FillerState *State = GetFillerState();
  const FillerLayer *Lower = &State->Layers[Cube->RowBand[Row]];
  int Top = Lower->Boundary + (Lower->Band - (Lower->Band / 2));
  float Chance = (float)(Top - Y) / (float)Lower->Band;
  float Roll = HashUnit(HashPosition(X, Y, Z, Cube->Seed ^ FILLER_BAND_SALT));
  return (Roll < Chance) ? Lower->Block
                         : State->Layers[Cube->RowBand[Row] - 1].Block;
}

bool FillerIsPocketBlock(BlockID BlockId) {
  const FillerState *State = GetFillerState();
  for (int Idx = 0; Idx < State->KindCount; Idx++) {
    if (State->Kinds[Idx].Block == BlockId) {
      return true;
    }
  }
  return false;
}
