#include "world/block_system.h"
#include "third_party/cJSON.h"
#include "core/fileio.h"
#include "core/utils.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  MAX_FILES_LIMIT = 1024
};

static int *GetLoadedBlocksCountPtr(void) {
  static int SLoadedBlocksCount = 0;
  return &SLoadedBlocksCount;
}

BlockType *GetBlockRegistry(void) {
  static BlockType SBlockRegistry[BLOCK_REGISTRY_SIZE];
  return SBlockRegistry;
}

void InitBlockRegistry(void) {
  BlockType *BlockRegistry = GetBlockRegistry();
  #pragma unroll 4
  for (int Idx = 0; Idx < BLOCK_REGISTRY_SIZE; Idx++) {
    BlockRegistry[Idx].Id = -1;
    BlockRegistry[Idx].Name[0] = '\0';
    BlockRegistry[Idx].TexTop = 0;
    BlockRegistry[Idx].TexSide = 0;
    BlockRegistry[Idx].TexBottom = 0;
    BlockRegistry[Idx].TexSideOverlay = NO_TEXTURE_OVERLAY;
    BlockRegistry[Idx].Tint = TINT_NONE;
    BlockRegistry[Idx].IsTransparent = true;
    BlockRegistry[Idx].IsSolid = true;
  }
}

// Optional "tint" property. An absent, non-string, or unrecognized value takes
// TINT_NONE rather than failing the block: an untinted block is a meaningful
// default, and the rest of the definition is still worth loading.
static TintSource ParseTintSource(const cJSON *Json) {
  cJSON *TintItem = cJSON_GetObjectItemCaseSensitive(Json, "tint");
  if (cJSON_IsString(TintItem) == 0) {
    return TINT_NONE;
  }
  if (CompareString(TintItem->valuestring, "grass") == 0) {
    return TINT_GRASS;
  }
  if (CompareString(TintItem->valuestring, "foliage") == 0) {
    return TINT_FOLIAGE;
  }
  return TINT_NONE;
}

BlockType *GetBlockDef(int BlockId) {
  BlockType *BlockRegistry = GetBlockRegistry();
  if (BlockId < 0 || BlockId >= BLOCK_REGISTRY_SIZE) {
    return &BlockRegistry[0];
  }

  if (BlockRegistry[BlockId].Id == -1) {
    return &BlockRegistry[0];
  }

  return &BlockRegistry[BlockId];
}

int GetBlockIdByName(const char *Name) {
  if (Name == NULL) {
    return -1;
  }

  BlockType *BlockRegistry = GetBlockRegistry();
  #pragma unroll 4
  for (int Idx = 0; Idx < BLOCK_REGISTRY_SIZE; Idx++) {
    if (BlockRegistry[Idx].Id != -1 &&
        CompareString(BlockRegistry[Idx].Name, Name) == 0) {
      return BlockRegistry[Idx].Id;
    }
  }

  return -1;
}

void ParseBlockFile(const char *FilePath) {
  char *FileContent = ReadTextFile(FilePath);
  if (FileContent == (void*)0) {
    fprintf(stderr, "BLOCK_SYSTEM: Failed to read file: %s\n", FilePath);
    return;
  }

  cJSON *Json = cJSON_Parse(FileContent);
  if (Json == (void*)0) {
    const char *ErrorPtr = cJSON_GetErrorPtr();
    fprintf(stderr, "BLOCK_SYSTEM: JSON syntax error %s before: %s\n",
            FilePath, ErrorPtr);
    free(FileContent);
    return;
  }

  cJSON *IdItem = cJSON_GetObjectItemCaseSensitive(Json, "id");
  if (cJSON_IsNumber(IdItem) == 0) {
    fprintf(stderr, "BLOCK_SYSTEM: FILE %s does not have a valid id\n",
            FilePath);
    cJSON_Delete(Json);
    free(FileContent);
    return;
  }

  int ParsedId = IdItem->valueint;
  if (ParsedId < 0 || ParsedId >= BLOCK_REGISTRY_SIZE) {
    fprintf(
        stderr,
        "BLOCK_SYSTEM: id %d from file %s is out of the allowed range (0-%d)\n",
        ParsedId, FilePath, BLOCK_REGISTRY_SIZE - 1);
    cJSON_Delete(Json);
    free(FileContent);
    return;
  }

  BlockType *BlockRegistry = GetBlockRegistry();
  BlockType *Block = &BlockRegistry[ParsedId];
  Block->Id = ParsedId;

  cJSON *NameItem = cJSON_GetObjectItemCaseSensitive(Json, "name");
  if (cJSON_IsString(NameItem) != 0) {
    SafeStrncpy(Block->Name, NameItem->valuestring, MAX_BLOCK_NAME_SIZE);
  }

  cJSON *TexTopItem = cJSON_GetObjectItemCaseSensitive(Json, "texTop");
  if (cJSON_IsNumber(TexTopItem) != 0) {
    Block->TexTop = TexTopItem->valueint;
  } else {
    Block->TexTop = 0;
  }

  cJSON *TexBottomItem = cJSON_GetObjectItemCaseSensitive(Json, "texBottom");
  if (cJSON_IsNumber(TexBottomItem) != 0) {
    Block->TexBottom = TexBottomItem->valueint;
  } else {
    Block->TexBottom = 0;
  }

  cJSON *TexSideItem = cJSON_GetObjectItemCaseSensitive(Json, "texSide");
  if (cJSON_IsNumber(TexSideItem) != 0) {
    Block->TexSide = TexSideItem->valueint;
  } else {
    Block->TexSide = 0;
  }

  cJSON *TexSideOverlayItem =
      cJSON_GetObjectItemCaseSensitive(Json, "texSideOverlay");
  if (cJSON_IsNumber(TexSideOverlayItem) != 0) {
    Block->TexSideOverlay = TexSideOverlayItem->valueint;
  } else {
    Block->TexSideOverlay = NO_TEXTURE_OVERLAY;
  }

  Block->Tint = ParseTintSource(Json);

  cJSON *IsBlockTransparent =
      cJSON_GetObjectItemCaseSensitive(Json, "isTransparent");
  if (cJSON_IsBool(IsBlockTransparent) != 0) {
    Block->IsTransparent = (cJSON_IsTrue(IsBlockTransparent) != 0);
  } else {
    Block->IsTransparent = false;
  }

  cJSON *IsBlockSolid = cJSON_GetObjectItemCaseSensitive(Json, "isSolid");
  if (cJSON_IsBool(IsBlockSolid) != 0) {
    Block->IsSolid = (cJSON_IsTrue(IsBlockSolid) != 0);
  } else {
    Block->IsSolid = true;
  }

  fprintf(stderr, "BLOCK_SYSTEM: Loaded [%d] %s\n", ParsedId, Block->Name);

  cJSON_Delete(Json);
  free(FileContent);
}

void LoadAllBlockDefinitions(const char *DirectoryPath) {
  char Path[FILEIO_MAX_PATH];
  snprintf(Path, sizeof(Path), "./%s", DirectoryPath);

  fprintf(stderr, "BLOCK_SYSTEM: Attempting to load from: %s\n", Path);

  static char FilePaths[MAX_FILES_LIMIT][FILEIO_MAX_PATH];
  int FilesCount = ListDirectoryFiles(Path, FilePaths, MAX_FILES_LIMIT);
  if (FilesCount == 0) {
    fprintf(stderr, "BLOCK_SYSTEM: Directory not found or empty %s\n", Path);
    return;
  }

  int LoadedCount = 0;

  #pragma unroll 4
  for (int Idx = 0; Idx < FilesCount; Idx++) {
    if (HasFileExtension(FilePaths[Idx], ".json")) {
      ParseBlockFile(FilePaths[Idx]);
      LoadedCount++;
    }
  }

  *GetLoadedBlocksCountPtr() = LoadedCount;

  if (LoadedCount == 0) {
    fprintf(stderr, "BLOCK_SYSTEM: No .json file found in %s\n", Path);
  } else {
    fprintf(stderr, "BLOCK_SYSTEM: Loaded %d block definitions\n", LoadedCount);
  }
}

int GetLoadedBlocksCount(void) { return *GetLoadedBlocksCountPtr(); }
