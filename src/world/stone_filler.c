#include "world/stone_filler.h"
#include "core/fileio.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/utils.h"
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
  BlockID DeepBlock;
  int MinY;
  int MaxY;
  float Chance;
  int RadiusMin;
  int RadiusMax;
  char Name[FILLER_MAX_ORE_NAME];
} PocketKind;

// One hashed-ellipsoid grid: the pocket grid or the ore-vein grid.
typedef struct {
  PocketKind Kinds[FILLER_MAX_ORE_KINDS];
  int KindCount;
  int Cell;
  uint64_t Salt;
} BlobGrid;

typedef struct {
  FillerLayer Layers[FILLER_MAX_LAYERS];
  int LayerCount;
  BlobGrid Pockets;
  BlobGrid Ores;
} FillerState;

#define FILLER_BAND_SALT 0xF111E40000000001ULL
#define FILLER_POCKET_SALT 0xF111E40000000002ULL
#define FILLER_ORE_SALT 0xF111E40000000003ULL

static FillerState *GetFillerState(void) {
  static FillerState SState;
  return &SState;
}

static void InstallStoneOnly(FillerState *State) {
  const int DEFAULT_POCKET_CELL = 32;
  const int DEFAULT_ORE_CELL = 16;
  int Stone = GetBlockIdByName("Stone");
  State->Layers[0] = (FillerLayer){
      .Block = (BlockID)((Stone > 0) ? Stone : 0), .Boundary = 0, .Band = 0};
  State->LayerCount = 1;
  State->Pockets.KindCount = 0;
  State->Pockets.Cell = DEFAULT_POCKET_CELL;
  State->Pockets.Salt = FILLER_POCKET_SALT;
  State->Ores.KindCount = 0;
  State->Ores.Cell = DEFAULT_ORE_CELL;
  State->Ores.Salt = FILLER_ORE_SALT;
}

void InitStoneFiller(void) { InstallStoneOnly(GetFillerState()); }

static bool ResolveNamedBlock(const cJSON *Item, const char *Key,
                              const char *FilePath, BlockID *Out) {
  const cJSON *Name = cJSON_GetObjectItemCaseSensitive(Item, Key);
  int Id = cJSON_IsString(Name) ? GetBlockIdByName(Name->valuestring) : -1;
  if (Id <= 0) {
    LogError("FILLER: %s names a block that is not loaded", FilePath);
    return false;
  }
  *Out = (BlockID)Id;
  return true;
}

static bool ResolveBlock(const cJSON *Item, const char *FilePath,
                         BlockID *Out) {
  return ResolveNamedBlock(Item, "block", FilePath, Out);
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

// A pocket or ore list: ListKey entries in a grid of CellKey-sized cells.
static bool ParseBlobGrid(const cJSON *Json, const char *ListKey,
                          const char *CellKey, int MaxKinds,
                          const char *FilePath, BlobGrid *Out) {
  Out->Cell = ReadIntOr(Json, CellKey, Out->Cell);
  const cJSON *List = cJSON_GetObjectItemCaseSensitive(Json, ListKey);
  int Count = cJSON_IsArray(List) ? cJSON_GetArraySize(List) : 0;
  if (Count > MaxKinds || Out->Cell < 2) {
    LogError("FILLER: %s needs at most %d %s kinds and a %s of 2 or more",
             FilePath, MaxKinds, ListKey, CellKey);
    return false;
  }
  for (int Idx = 0; Idx < Count; Idx++) {
    const cJSON *Item = cJSON_GetArrayItem(List, Idx);
    PocketKind *Kind = &Out->Kinds[Idx];
    if (!ResolveBlock(Item, FilePath, &Kind->Block)) {
      return false;
    }
    Kind->DeepBlock = 0;
    if (cJSON_GetObjectItemCaseSensitive(Item, "deepslateBlock") != NULL &&
        !ResolveNamedBlock(Item, "deepslateBlock", FilePath,
                           &Kind->DeepBlock)) {
      return false;
    }
    const cJSON *Name = cJSON_GetObjectItemCaseSensitive(Item, "block");
    SafeStrncpy(Kind->Name, Name->valuestring, FILLER_MAX_ORE_NAME);
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
        Kind->RadiusMax * 2 > Out->Cell || Kind->MaxY < Kind->MinY) {
      LogError("FILLER: %s %s entry %d needs 1 <= radius min <= max <= "
               "%s / 2 and minY <= maxY",
               FilePath, ListKey, Idx, CellKey);
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
      ParseBlobGrid(Json, "pockets", "pocketCell", FILLER_MAX_POCKET_KINDS,
                    FilePath, &Parsed.Pockets) &&
      ParseBlobGrid(Json, "ores", "oreCell", FILLER_MAX_ORE_KINDS, FilePath,
                    &Parsed.Ores)) {
    *State = Parsed;
    LogInfo("FILLER: %d layers, %d pocket kinds (cell %d), %d ore kinds "
            "(cell %d)",
            State->LayerCount, State->Pockets.KindCount, State->Pockets.Cell,
            State->Ores.KindCount, State->Ores.Cell);
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

// The blob of one cell of Grid, if the cell has one.
static bool CellPocket(const BlobGrid *Grid, int CellX, int CellY, int CellZ,
                       uint64_t Seed, FillerPocket *Out) {
  int Size = Grid->Cell;
  int CenterY = (CellY * Size) + (Size / 2);
  int Eligible[FILLER_MAX_ORE_KINDS];
  int EligibleCount = 0;
  for (int Idx = 0; Idx < Grid->KindCount; Idx++) {
    if (CenterY >= Grid->Kinds[Idx].MinY && CenterY <= Grid->Kinds[Idx].MaxY) {
      Eligible[EligibleCount++] = Idx;
    }
  }
  if (EligibleCount == 0) {
    return false;
  }

  uint64_t Hash = HashPosition(CellX, CellY, CellZ, Seed ^ Grid->Salt);
  const PocketKind *Kind =
      &Grid->Kinds[Eligible[SplitMix64(Hash) % (uint64_t)EligibleCount]];
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
  Out->DeepBlock = Kind->DeepBlock;
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

// The blobs of Grid's 3 x 3 x 3 cells around the cube that reach it. A blob's
// radius is at most half its cell, so no farther cell can reach.
static int CollectBlobs(const BlobGrid *Grid, int MinX, int MinY, int MinZ,
                        uint64_t Seed,
                        FillerPocket Out[FILLER_MAX_CUBE_POCKETS]) {
  if (Grid->KindCount == 0) {
    return 0;
  }
  int Count = 0;
  int Size = Grid->Cell;
  int Center[3] = {FloorDiv(MinX, Size), FloorDiv(MinY, Size),
                   FloorDiv(MinZ, Size)};
  for (int Dx = -1; Dx <= 1; Dx++) {
    for (int Dy = -1; Dy <= 1; Dy++) {
      for (int Dz = -1; Dz <= 1; Dz++) {
        FillerPocket Blob;
        if (CellPocket(Grid, Center[0] + Dx, Center[1] + Dy, Center[2] + Dz,
                       Seed, &Blob) &&
            PocketReachesCube(&Blob, MinX, MinY, MinZ)) {
          Out[Count++] = Blob;
        }
      }
    }
  }
  return Count;
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

  Cube->PocketCount =
      CollectBlobs(&State->Pockets, MinX, MinY, MinZ, Seed, Cube->Pockets);
  Cube->VeinCount =
      CollectBlobs(&State->Ores, MinX, MinY, MinZ, Seed, Cube->Veins);
}

bool FillerCubeUniform(const FillerCube *Cube, BlockID *OutBlock) {
  if (Cube->PocketCount > 0 || Cube->VeinCount > 0) {
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
  // Integer box first: most tests miss a blob entirely.
  if (Y < Pocket->MinY || Y > Pocket->MaxY ||
      abs(X - Pocket->CenterX) > Pocket->RadiusX ||
      abs(Y - Pocket->CenterY) > Pocket->RadiusY ||
      abs(Z - Pocket->CenterZ) > Pocket->RadiusZ) {
    return false;
  }
  float Dx = (float)(X - Pocket->CenterX) / (float)Pocket->RadiusX;
  float Dy = (float)(Y - Pocket->CenterY) / (float)Pocket->RadiusY;
  float Dz = (float)(Z - Pocket->CenterZ) / (float)Pocket->RadiusZ;
  return (Dx * Dx) + (Dy * Dy) + (Dz * Dz) <= 1.0F;
}

// Layer filler at a position: the row's block, or the band's hashed choice.
static BlockID LayerBlockAt(const FillerCube *Cube, int X, int Y, int Z) {
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

BlockID FillerBlockAt(const FillerCube *Cube, int X, int Y, int Z) {
  for (int Idx = 0; Idx < Cube->PocketCount; Idx++) {
    if (InsidePocket(&Cube->Pockets[Idx], X, Y, Z)) {
      return Cube->Pockets[Idx].Block;
    }
  }
  BlockID Layer = LayerBlockAt(Cube, X, Y, Z);
  for (int Idx = 0; Idx < Cube->VeinCount; Idx++) {
    const FillerPocket *Vein = &Cube->Veins[Idx];
    if (InsidePocket(Vein, X, Y, Z)) {
      const FillerState *State = GetFillerState();
      BlockID Deepest = State->Layers[State->LayerCount - 1].Block;
      bool Deep = State->LayerCount > 1 && Layer == Deepest;
      return (Deep && Vein->DeepBlock != 0) ? Vein->DeepBlock : Vein->Block;
    }
  }
  return Layer;
}

// Writes Blob's cells over Layer into a uniform cube, never over a cell that
// already differs from Layer (an earlier pocket or vein, which wins as it does
// in FillerBlockAt).
static void OverlayBlob(const FillerPocket *Blob, BlockID Block, BlockID Layer,
                        int OriginX, int OriginY, int OriginZ,
                        BlockID Data[FILLER_ROWS][FILLER_ROWS][FILLER_ROWS]) {
  int Lo[3] = {Blob->CenterX - Blob->RadiusX - OriginX,
               Blob->CenterY - Blob->RadiusY - OriginY,
               Blob->CenterZ - Blob->RadiusZ - OriginZ};
  int Hi[3] = {Blob->CenterX + Blob->RadiusX - OriginX,
               Blob->CenterY + Blob->RadiusY - OriginY,
               Blob->CenterZ + Blob->RadiusZ - OriginZ};
  for (int Axis = 0; Axis < 3; Axis++) {
    Lo[Axis] = (Lo[Axis] < 0) ? 0 : Lo[Axis];
    Hi[Axis] = (Hi[Axis] > FILLER_ROWS - 1) ? FILLER_ROWS - 1 : Hi[Axis];
  }
  for (int Lx = Lo[0]; Lx <= Hi[0]; Lx++) {
    for (int Ly = Lo[1]; Ly <= Hi[1]; Ly++) {
      for (int Lz = Lo[2]; Lz <= Hi[2]; Lz++) {
        if (Data[Lx][Ly][Lz] == Layer &&
            InsidePocket(Blob, OriginX + Lx, OriginY + Ly, OriginZ + Lz)) {
          Data[Lx][Ly][Lz] = Block;
        }
      }
    }
  }
}

void FillerFillCube(const FillerCube *Cube, int OriginX, int OriginZ,
                    BlockID Data[FILLER_ROWS][FILLER_ROWS][FILLER_ROWS]) {
  bool Uniform = true;
  for (int Row = 0; Row < FILLER_ROWS; Row++) {
    Uniform &= Cube->RowBlock[Row] != 0 &&
               Cube->RowBlock[Row] == Cube->RowBlock[0];
  }
  int OriginY = Cube->BaseY;
  if (!Uniform) {
    for (int Lx = 0; Lx < FILLER_ROWS; Lx++) {
      for (int Ly = 0; Ly < FILLER_ROWS; Ly++) {
        for (int Lz = 0; Lz < FILLER_ROWS; Lz++) {
          Data[Lx][Ly][Lz] =
              FillerBlockAt(Cube, OriginX + Lx, OriginY + Ly, OriginZ + Lz);
        }
      }
    }
    return;
  }

  // One layer block throughout: fill it, then draw only the blob cells.
  // Pockets go first and veins after, never over a pocket, exactly the
  // priority FillerBlockAt applies block by block.
  BlockID Layer = Cube->RowBlock[0];
  for (int Lx = 0; Lx < FILLER_ROWS; Lx++) {
    for (int Ly = 0; Ly < FILLER_ROWS; Ly++) {
      for (int Lz = 0; Lz < FILLER_ROWS; Lz++) {
        Data[Lx][Ly][Lz] = Layer;
      }
    }
  }
  for (int Idx = 0; Idx < Cube->PocketCount; Idx++) {
    OverlayBlob(&Cube->Pockets[Idx], Cube->Pockets[Idx].Block, Layer, OriginX,
                OriginY, OriginZ, Data);
  }
  const FillerState *State = GetFillerState();
  bool Deep = State->LayerCount > 1 &&
              Layer == State->Layers[State->LayerCount - 1].Block;
  for (int Idx = 0; Idx < Cube->VeinCount; Idx++) {
    const FillerPocket *Vein = &Cube->Veins[Idx];
    BlockID Ore = (Deep && Vein->DeepBlock != 0) ? Vein->DeepBlock : Vein->Block;
    OverlayBlob(Vein, Ore, Layer, OriginX, OriginY, OriginZ, Data);
  }
}

bool FillerIsPocketBlock(BlockID BlockId) {
  const BlobGrid *Grid = &GetFillerState()->Pockets;
  for (int Idx = 0; Idx < Grid->KindCount; Idx++) {
    if (Grid->Kinds[Idx].Block == BlockId) {
      return true;
    }
  }
  return false;
}

int FillerOreKindCount(void) { return GetFillerState()->Ores.KindCount; }

const char *FillerOreKindName(int Kind) {
  const BlobGrid *Grid = &GetFillerState()->Ores;
  return (Kind >= 0 && Kind < Grid->KindCount) ? Grid->Kinds[Kind].Name : "";
}

int FillerOreKindOf(BlockID BlockId) {
  const BlobGrid *Grid = &GetFillerState()->Ores;
  for (int Idx = 0; Idx < Grid->KindCount; Idx++) {
    if (BlockId != 0 && (Grid->Kinds[Idx].Block == BlockId ||
                         Grid->Kinds[Idx].DeepBlock == BlockId)) {
      return Idx;
    }
  }
  return -1;
}
