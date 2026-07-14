#include "world/prefab_capture.h"
#include "world/prefab.h"
#include "world/block_system.h"
#include "world/world.h"
#include "core/fileio.h"
#include "core/utils.h"
#include "core/log.h"
#include "third_party/cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
  CAPTURE_MAX_AXIS_SIZE = 255,
  CAPTURE_MAX_PALETTE = 256,
  CAPTURE_CELL_FIELDS = 4,
  CAPTURE_AXIS_COUNT = 3
};

static void SetError(char *OutError, int ErrorSize, const char *Message) {
  if (OutError != NULL && ErrorSize > 0) {
    SafeStrncpy(OutError, Message, ErrorSize);
  }
}

// Reject empty names, names that would escape the assets directory, and names
// that do not fit the prefab name field.
static bool IsValidPrefabName(const char *Name) {
  if (Name == NULL || Name[0] == '\0') {
    return false;
  }
  size_t Length = strlen(Name);
  if (Length >= MAX_PREFAB_NAME_SIZE) {
    return false;
  }
  for (size_t Idx = 0; Idx < Length; Idx++) {
    char Char = Name[Idx];
    if (Char == '/' || Char == '\\' || Char == ':') {
      return false;
    }
  }
  return true;
}

// Find BlockId in the first-seen palette, appending it when absent. Returns the
// palette index, or -1 when the palette is full.
static int PaletteIndexOf(int *PaletteIds, int *PaletteCount, int BlockId) {
  for (int Idx = 0; Idx < *PaletteCount; Idx++) {
    if (PaletteIds[Idx] == BlockId) {
      return Idx;
    }
  }
  if (*PaletteCount >= CAPTURE_MAX_PALETTE) {
    return -1;
  }
  int NewIndex = *PaletteCount;
  PaletteIds[NewIndex] = BlockId;
  (*PaletteCount)++;
  return NewIndex;
}

bool CaptureSelectionToPrefab(struct World *World, Vec3 CornerA, Vec3 CornerB,
                              const Vec3 *Offset, const char *Name,
                              char *OutError, int ErrorSize) {
  if (World == NULL) {
    SetError(OutError, ErrorSize, "No world");
    return false;
  }
  if (IsValidPrefabName(Name) == false) {
    SetError(OutError, ErrorSize, "Invalid prefab name");
    return false;
  }

  int MinX = (int)floorf(fminf(CornerA.x, CornerB.x));
  int MinY = (int)floorf(fminf(CornerA.y, CornerB.y));
  int MinZ = (int)floorf(fminf(CornerA.z, CornerB.z));
  int MaxX = (int)floorf(fmaxf(CornerA.x, CornerB.x));
  int MaxY = (int)floorf(fmaxf(CornerA.y, CornerB.y));
  int MaxZ = (int)floorf(fmaxf(CornerA.z, CornerB.z));

  int SizeX = MaxX - MinX + 1;
  int SizeY = MaxY - MinY + 1;
  int SizeZ = MaxZ - MinZ + 1;

  if (SizeX > CAPTURE_MAX_AXIS_SIZE || SizeY > CAPTURE_MAX_AXIS_SIZE ||
      SizeZ > CAPTURE_MAX_AXIS_SIZE) {
    SetError(OutError, ErrorSize, "Selection too large");
    return false;
  }

  cJSON *Root = cJSON_CreateObject();
  cJSON *BlocksArray = cJSON_CreateArray();
  if (Root == NULL || BlocksArray == NULL) {
    cJSON_Delete(Root);
    cJSON_Delete(BlocksArray);
    SetError(OutError, ErrorSize, "Out of memory");
    return false;
  }

  int PaletteIds[CAPTURE_MAX_PALETTE];
  int PaletteCount = 0;

  for (int Y = MinY; Y <= MaxY; Y++) {
    for (int Z = MinZ; Z <= MaxZ; Z++) {
      for (int X = MinX; X <= MaxX; X++) {
        Vec3 Pos = {(float)X, (float)Y, (float)Z};
        int BlockId = GetBlockIDFromWorld(World, Pos);
        if (BlockId <= 0) {
          continue;
        }
        int PaletteIndex = PaletteIndexOf(PaletteIds, &PaletteCount, BlockId);
        if (PaletteIndex < 0) {
          cJSON_Delete(Root);
          cJSON_Delete(BlocksArray);
          SetError(OutError, ErrorSize, "Too many block types");
          return false;
        }
        int Cell[CAPTURE_CELL_FIELDS] = {X - MinX, Y - MinY, Z - MinZ,
                                         PaletteIndex};
        cJSON *CellArray = cJSON_CreateIntArray(Cell, CAPTURE_CELL_FIELDS);
        if (CellArray == NULL ||
            cJSON_AddItemToArray(BlocksArray, CellArray) == 0) {
          cJSON_Delete(CellArray);
          cJSON_Delete(Root);
          cJSON_Delete(BlocksArray);
          SetError(OutError, ErrorSize, "Out of memory");
          return false;
        }
      }
    }
  }

  const char *PaletteNames[CAPTURE_MAX_PALETTE];
  for (int Idx = 0; Idx < PaletteCount; Idx++) {
    BlockType *Def = GetBlockDef(PaletteIds[Idx]);
    PaletteNames[Idx] = (Def != NULL) ? Def->Name : "";
  }

  int SizeDims[CAPTURE_AXIS_COUNT] = {SizeX, SizeY, SizeZ};
  cJSON_AddStringToObject(Root, "name", Name);
  cJSON_AddItemToObject(Root, "size",
                        cJSON_CreateIntArray(SizeDims, CAPTURE_AXIS_COUNT));
  if (Offset != NULL) {
    int OriginLocal[CAPTURE_AXIS_COUNT] = {(int)floorf(Offset->x) - MinX,
                                           (int)floorf(Offset->y) - MinY,
                                           (int)floorf(Offset->z) - MinZ};
    cJSON_AddItemToObject(
        Root, "origin", cJSON_CreateIntArray(OriginLocal, CAPTURE_AXIS_COUNT));
  }
  cJSON_AddItemToObject(Root, "palette",
                        cJSON_CreateStringArray(PaletteNames, PaletteCount));
  cJSON_AddItemToObject(Root, "blocks", BlocksArray);

  char *Text = cJSON_Print(Root);
  cJSON_Delete(Root);
  if (Text == NULL) {
    SetError(OutError, ErrorSize, "Serialization failed");
    return false;
  }

  char Path[FILEIO_MAX_PATH];
  snprintf(Path, sizeof(Path), "assets/prefabs/%s.json", Name);
  bool Written = WriteTextFile(Path, Text);
  cJSON_free(Text);

  if (Written == false) {
    SetError(OutError, ErrorSize, "Failed to write file");
    return false;
  }

  LogInfo("PREFAB: Captured '%s' (%dx%dx%d) to %s", Name, SizeX, SizeY, SizeZ,
          Path);

  // Reload the freshly written file so the capture is stampable this session.
  if (RegisterPrefabFile(Path) == false) {
    SetError(OutError, ErrorSize, "Saved but failed to reload");
    return false;
  }
  return true;
}
