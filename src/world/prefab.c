#include "world/prefab.h"
#include "world/block_system.h"
#include "world/chunk.h"
#include "world/world.h"
#include "third_party/cJSON.h"
#include "core/fileio.h"
#include "core/utils.h"
#include "core/log.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  MAX_FILES_LIMIT = 1024,
  MAX_PALETTE_SIZE = 256,
  MAX_AXIS_SIZE = 255,
  CELL_FIELD_COUNT = 4
};

static Prefab *GetPrefabRegistry(void) {
  static Prefab SPrefabRegistry[MAX_PREFABS];
  return SPrefabRegistry;
}

static int *GetPrefabCountPtr(void) {
  static int SPrefabCount = 0;
  return &SPrefabCount;
}

void InitPrefabRegistry(void) {
  Prefab *Registry = GetPrefabRegistry();
  for (int Idx = 0; Idx < MAX_PREFABS; Idx++) {
    if (Registry[Idx].Cells != NULL) {
      free(Registry[Idx].Cells);
    }
    Registry[Idx].Name[0] = '\0';
    Registry[Idx].SizeX = 0;
    Registry[Idx].SizeY = 0;
    Registry[Idx].SizeZ = 0;
    Registry[Idx].OriginX = 0;
    Registry[Idx].OriginY = 0;
    Registry[Idx].OriginZ = 0;
    Registry[Idx].Cells = NULL;
    Registry[Idx].CellCount = 0;
    Registry[Idx].HasOrigin = false;
    Registry[Idx].AllowRotation = true;
    Registry[Idx].AllowMirror = true;
  }
  *GetPrefabCountPtr() = 0;
}

const Prefab *GetPrefab(const char *Name) {
  if (Name == NULL) {
    return NULL;
  }

  Prefab *Registry = GetPrefabRegistry();
  int Count = *GetPrefabCountPtr();
  for (int Idx = 0; Idx < Count; Idx++) {
    if (CompareString(Registry[Idx].Name, Name) == 0) {
      return &Registry[Idx];
    }
  }

  return NULL;
}

const Prefab *GetPrefabByIndex(int Index) {
  if (Index < 0 || Index >= *GetPrefabCountPtr()) {
    return NULL;
  }
  return &GetPrefabRegistry()[Index];
}

int GetLoadedPrefabsCount(void) { return *GetPrefabCountPtr(); }

// Read three positive axis sizes from the "size" array. Returns false on any
// malformed or out-of-range value.
static bool ParseSize(const cJSON *SizeItem, int *OutX, int *OutY, int *OutZ) {
  if (cJSON_IsArray(SizeItem) == 0 || cJSON_GetArraySize(SizeItem) != 3) {
    return false;
  }

  int Dims[3];
  for (int Idx = 0; Idx < 3; Idx++) {
    const cJSON *DimItem = cJSON_GetArrayItem(SizeItem, Idx);
    if (cJSON_IsNumber(DimItem) == 0) {
      return false;
    }
    int Dim = DimItem->valueint;
    if (Dim <= 0 || Dim > MAX_AXIS_SIZE) {
      return false;
    }
    Dims[Idx] = Dim;
  }

  *OutX = Dims[0];
  *OutY = Dims[1];
  *OutZ = Dims[2];
  return true;
}

// Resolve each palette name to a block id via the registry. Unresolved names
// become -1 so referencing cells can be skipped. Returns the palette size, or
// -1 when the palette is missing or too large.
static int ResolvePalette(const cJSON *PaletteItem, int *OutIds,
                          const char *FilePath) {
  if (cJSON_IsArray(PaletteItem) == 0) {
    return -1;
  }

  int PaletteSize = cJSON_GetArraySize(PaletteItem);
  if (PaletteSize <= 0 || PaletteSize > MAX_PALETTE_SIZE) {
    return -1;
  }

  for (int Idx = 0; Idx < PaletteSize; Idx++) {
    const cJSON *NameItem = cJSON_GetArrayItem(PaletteItem, Idx);
    if (cJSON_IsString(NameItem) == 0) {
      OutIds[Idx] = -1;
      continue;
    }
    int BlockId = GetBlockIdByName(NameItem->valuestring);
    if (BlockId < 0) {
      LogWarn("PREFAB: %s palette entry '%s' is not a loaded block", FilePath,
              NameItem->valuestring);
    }
    OutIds[Idx] = BlockId;
  }

  return PaletteSize;
}

// Compile one cell array [x, y, z, paletteIndex] into OutCell. Returns false
// when the cell is malformed, out of bounds, or references an unresolved
// palette entry.
static bool ParseCell(const cJSON *CellItem, int SizeX, int SizeY, int SizeZ,
                      const int *PaletteIds, int PaletteSize,
                      PrefabCell *OutCell) {
  if (cJSON_IsArray(CellItem) == 0 ||
      cJSON_GetArraySize(CellItem) < CELL_FIELD_COUNT) {
    return false;
  }

  int Fields[CELL_FIELD_COUNT];
  for (int Idx = 0; Idx < CELL_FIELD_COUNT; Idx++) {
    const cJSON *FieldItem = cJSON_GetArrayItem(CellItem, Idx);
    if (cJSON_IsNumber(FieldItem) == 0) {
      return false;
    }
    Fields[Idx] = FieldItem->valueint;
  }

  int PosX = Fields[0];
  int PosY = Fields[1];
  int PosZ = Fields[2];
  int PaletteIndex = Fields[3];

  if (PosX < 0 || PosX >= SizeX || PosY < 0 || PosY >= SizeY || PosZ < 0 ||
      PosZ >= SizeZ) {
    return false;
  }
  if (PaletteIndex < 0 || PaletteIndex >= PaletteSize) {
    return false;
  }

  int BlockId = PaletteIds[PaletteIndex];
  if (BlockId < 0) {
    return false;
  }

  OutCell->X = (unsigned char)PosX;
  OutCell->Y = (unsigned char)PosY;
  OutCell->Z = (unsigned char)PosZ;
  OutCell->BlockId = (unsigned char)BlockId;
  return true;
}

// Read an optional [ox, oy, oz] anchor, requiring each value to fall within the
// declared size. Returns false when present but malformed or out of range.
static bool ParseOrigin(const cJSON *OriginItem, int SizeX, int SizeY,
                        int SizeZ, int *OutX, int *OutY, int *OutZ) {
  if (cJSON_IsArray(OriginItem) == 0 || cJSON_GetArraySize(OriginItem) != 3) {
    return false;
  }

  int Coords[3];
  int Bounds[3] = {SizeX, SizeY, SizeZ};
  for (int Idx = 0; Idx < 3; Idx++) {
    const cJSON *Item = cJSON_GetArrayItem(OriginItem, Idx);
    if (cJSON_IsNumber(Item) == 0) {
      return false;
    }
    int Value = Item->valueint;
    if (Value < 0 || Value >= Bounds[Idx]) {
      return false;
    }
    Coords[Idx] = Value;
  }

  *OutX = Coords[0];
  *OutY = Coords[1];
  *OutZ = Coords[2];
  return true;
}

// Read an optional boolean flag, defaulting to true when absent. A present but
// non-boolean value is warned about and falls back to the default, mirroring the
// tolerant parse used for the optional origin.
static bool ParseOptionalBool(const cJSON *Item, const char *FieldName,
                              const char *FilePath) {
  if (Item == NULL) {
    return true;
  }
  if (cJSON_IsBool(Item) == 0) {
    LogWarn("PREFAB: %s has a non-boolean '%s', defaulting to true", FilePath,
            FieldName);
    return true;
  }
  return cJSON_IsTrue(Item) != 0;
}

// Locate a registered prefab slot by name. Returns its index, or -1 if absent.
static int FindPrefabSlot(const char *Name) {
  Prefab *Registry = GetPrefabRegistry();
  int Count = *GetPrefabCountPtr();
  for (int Idx = 0; Idx < Count; Idx++) {
    if (CompareString(Registry[Idx].Name, Name) == 0) {
      return Idx;
    }
  }
  return -1;
}

int GetPrefabIndexByName(const char *Name) {
  if (Name == NULL) {
    return -1;
  }
  return FindPrefabSlot(Name);
}

bool RegisterPrefabFile(const char *FilePath) {
  char *FileContent = ReadTextFile(FilePath);
  if (FileContent == NULL) {
    LogError("PREFAB: failed to read file: %s", FilePath);
    return false;
  }

  cJSON *Json = cJSON_Parse(FileContent);
  if (Json == NULL) {
    LogError("PREFAB: JSON syntax error in %s before: %s", FilePath,
             cJSON_GetErrorPtr());
    free(FileContent);
    return false;
  }

  const cJSON *NameItem = cJSON_GetObjectItemCaseSensitive(Json, "name");
  const cJSON *SizeItem = cJSON_GetObjectItemCaseSensitive(Json, "size");
  const cJSON *PaletteItem = cJSON_GetObjectItemCaseSensitive(Json, "palette");
  const cJSON *BlocksItem = cJSON_GetObjectItemCaseSensitive(Json, "blocks");
  const cJSON *OriginItem = cJSON_GetObjectItemCaseSensitive(Json, "origin");
  const cJSON *AllowRotationItem =
      cJSON_GetObjectItemCaseSensitive(Json, "allowRotation");
  const cJSON *AllowMirrorItem =
      cJSON_GetObjectItemCaseSensitive(Json, "allowMirror");

  int SizeX = 0;
  int SizeY = 0;
  int SizeZ = 0;
  int PaletteIds[MAX_PALETTE_SIZE];

  if (cJSON_IsString(NameItem) == 0) {
    LogError("PREFAB: %s missing a valid 'name'", FilePath);
    cJSON_Delete(Json);
    free(FileContent);
    return false;
  }
  if (ParseSize(SizeItem, &SizeX, &SizeY, &SizeZ) == false) {
    LogError("PREFAB: %s has an invalid 'size'", FilePath);
    cJSON_Delete(Json);
    free(FileContent);
    return false;
  }
  int PaletteSize = ResolvePalette(PaletteItem, PaletteIds, FilePath);
  if (PaletteSize < 0) {
    LogError("PREFAB: %s has an invalid 'palette'", FilePath);
    cJSON_Delete(Json);
    free(FileContent);
    return false;
  }
  if (cJSON_IsArray(BlocksItem) == 0) {
    LogError("PREFAB: %s has an invalid 'blocks' array", FilePath);
    cJSON_Delete(Json);
    free(FileContent);
    return false;
  }

  int BlockEntries = cJSON_GetArraySize(BlocksItem);
  PrefabCell *Cells = NULL;
  if (BlockEntries > 0) {
    Cells = (PrefabCell *)malloc(sizeof(PrefabCell) * (size_t)BlockEntries);
    if (Cells == NULL) {
      LogError("PREFAB: out of memory compiling %s", FilePath);
      cJSON_Delete(Json);
      free(FileContent);
      return false;
    }
  }

  int CellCount = 0;
  for (int Idx = 0; Idx < BlockEntries; Idx++) {
    const cJSON *CellItem = cJSON_GetArrayItem(BlocksItem, Idx);
    PrefabCell Cell;
    if (ParseCell(CellItem, SizeX, SizeY, SizeZ, PaletteIds, PaletteSize,
                  &Cell)) {
      Cells[CellCount] = Cell;
      CellCount++;
    } else {
      LogWarn("PREFAB: %s cell %d is invalid and was skipped", FilePath, Idx);
    }
  }

  int OriginX = 0;
  int OriginY = 0;
  int OriginZ = 0;
  bool HasOrigin = false;
  if (OriginItem != NULL) {
    if (ParseOrigin(OriginItem, SizeX, SizeY, SizeZ, &OriginX, &OriginY,
                    &OriginZ)) {
      HasOrigin = true;
    } else {
      LogWarn("PREFAB: %s has an invalid 'origin', ignoring it", FilePath);
    }
  }

  // Replace an existing prefab of the same name in place, otherwise append a
  // new slot. Replacing frees the old cell array first.
  Prefab *Registry = GetPrefabRegistry();
  int *CountPtr = GetPrefabCountPtr();
  int SlotIndex = FindPrefabSlot(NameItem->valuestring);
  bool Replacing = (SlotIndex >= 0);

  if (Replacing == false) {
    if (*CountPtr >= MAX_PREFABS) {
      LogWarn("PREFAB: registry full, skipping %s", FilePath);
      free(Cells);
      cJSON_Delete(Json);
      free(FileContent);
      return false;
    }
    SlotIndex = *CountPtr;
  }

  Prefab *Slot = &Registry[SlotIndex];
  if (Replacing && Slot->Cells != NULL) {
    free(Slot->Cells);
  }
  SafeStrncpy(Slot->Name, NameItem->valuestring, MAX_PREFAB_NAME_SIZE);
  Slot->SizeX = SizeX;
  Slot->SizeY = SizeY;
  Slot->SizeZ = SizeZ;
  Slot->OriginX = OriginX;
  Slot->OriginY = OriginY;
  Slot->OriginZ = OriginZ;
  Slot->Cells = Cells;
  Slot->CellCount = CellCount;
  Slot->HasOrigin = HasOrigin;
  Slot->AllowRotation =
      ParseOptionalBool(AllowRotationItem, "allowRotation", FilePath);
  Slot->AllowMirror = ParseOptionalBool(AllowMirrorItem, "allowMirror", FilePath);
  if (Replacing == false) {
    (*CountPtr)++;
  }

  LogInfo("PREFAB: %s '%s' (%dx%dx%d, %d cells)",
          Replacing ? "Replaced" : "Loaded", Slot->Name, SizeX, SizeY, SizeZ,
          CellCount);

  cJSON_Delete(Json);
  free(FileContent);
  return true;
}

void StampPrefab(struct World *WorldVal, const Prefab *PrefabVal, Vec3 Origin) {
  if (WorldVal == NULL || PrefabVal == NULL) {
    return;
  }

  for (int Idx = 0; Idx < PrefabVal->CellCount; Idx++) {
    PrefabCell Cell = PrefabVal->Cells[Idx];
    Vec3 Pos = {Origin.x + (float)Cell.X, Origin.y + (float)Cell.Y,
                Origin.z + (float)Cell.Z};
    SetBlockInWorld(WorldVal, Pos, Cell.BlockId);
  }
}

// Apply an orientation code (bits 0-1 rotation, bit 2 mirror) to a horizontal
// offset measured from the anchor column. The mirror flips local X first, then
// the rotation turns the offset about the vertical axis. Heights never pass
// through here.
static void OrientOffset(int Orientation, int Dx, int Dz, int *OutDx,
                         int *OutDz) {
  const int MIRROR_BIT = 0x4;
  const int ROTATION_MASK = 0x3;

  if ((Orientation & MIRROR_BIT) != 0) {
    Dx = -Dx;
  }

  switch (Orientation & ROTATION_MASK) {
  case 1: // 90 degrees
    *OutDx = -Dz;
    *OutDz = Dx;
    break;
  case 2: // 180 degrees
    *OutDx = -Dx;
    *OutDz = -Dz;
    break;
  case 3: // 270 degrees
    *OutDx = Dz;
    *OutDz = -Dx;
    break;
  default: // 0 degrees
    *OutDx = Dx;
    *OutDz = Dz;
    break;
  }
}

void StampPrefabIntoChunk(struct Chunk *ChunkVal, const Prefab *PrefabVal,
                          int WorldOriginX, int WorldOriginY, int WorldOriginZ,
                          int Orientation) {
  if (ChunkVal == NULL || PrefabVal == NULL) {
    return;
  }

  // Resolve the anchor cell inside the prefab: an explicit origin, otherwise the
  // horizontal center with the base at Y so the trunk lands on the chosen
  // column. The anchor maps to (WorldOriginX, WorldOriginZ) and is the pivot the
  // orientation rotates every other cell about.
  int AnchorX = 0;
  int AnchorY = 0;
  int AnchorZ = 0;
  if (PrefabVal->HasOrigin) {
    AnchorX = PrefabVal->OriginX;
    AnchorY = PrefabVal->OriginY;
    AnchorZ = PrefabVal->OriginZ;
  } else {
    AnchorX = PrefabVal->SizeX / 2;
    AnchorY = 0;
    AnchorZ = PrefabVal->SizeZ / 2;
  }

  int ChunkOriginX = ChunkVal->ChunkX * CHUNK_SIZE;
  int ChunkOriginY = ChunkVal->ChunkY * CHUNK_SIZE;
  int ChunkOriginZ = ChunkVal->ChunkZ * CHUNK_SIZE;

  for (int Idx = 0; Idx < PrefabVal->CellCount; Idx++) {
    PrefabCell Cell = PrefabVal->Cells[Idx];

    // Offset from the anchor, oriented horizontally; Y is unaffected.
    int Dx = 0;
    int Dz = 0;
    OrientOffset(Orientation, (int)Cell.X - AnchorX, (int)Cell.Z - AnchorZ, &Dx,
                 &Dz);

    int LocalX = (WorldOriginX + Dx) - ChunkOriginX;
    int LocalY = (WorldOriginY + ((int)Cell.Y - AnchorY)) - ChunkOriginY;
    int LocalZ = (WorldOriginZ + Dz) - ChunkOriginZ;

    if (LocalX < 0 || LocalX >= CHUNK_SIZE || LocalY < 0 ||
        LocalY >= CHUNK_SIZE || LocalZ < 0 || LocalZ >= CHUNK_SIZE) {
      continue;
    }

    // Air is structurally 0; keep any terrain block already occupying the cell.
    if (ChunkVal->Data[LocalX][LocalY][LocalZ] != 0) {
      continue;
    }

    ChunkVal->Data[LocalX][LocalY][LocalZ] = Cell.BlockId;
    ChunkVal->SolidBlockCount++;
  }
}

void LoadAllPrefabs(const char *DirectoryPath) {
  char Path[FILEIO_MAX_PATH];
  snprintf(Path, sizeof(Path), "./%s", DirectoryPath);

  LogInfo("PREFAB: Attempting to load from: %s", Path);

  static char FilePaths[MAX_FILES_LIMIT][FILEIO_MAX_PATH];
  int FilesCount = ListDirectoryFiles(Path, FilePaths, MAX_FILES_LIMIT);
  if (FilesCount == 0) {
    LogWarn("PREFAB: Directory not found or empty %s", Path);
    return;
  }

  for (int Idx = 0; Idx < FilesCount; Idx++) {
    if (HasFileExtension(FilePaths[Idx], ".json")) {
      RegisterPrefabFile(FilePaths[Idx]);
    }
  }

  int LoadedCount = *GetPrefabCountPtr();
  if (LoadedCount == 0) {
    LogWarn("PREFAB: No valid prefab loaded from %s", Path);
  } else {
    LogInfo("PREFAB: Loaded %d prefabs", LoadedCount);
  }
}
