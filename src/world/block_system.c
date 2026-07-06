#include "world/block_system.h"
#include "third_party/cJSON.h"
#include "raylib.h"
#include "core/utils.h"

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
    BlockRegistry[Idx].IsTransparent = true;
    BlockRegistry[Idx].IsSolid = true;
  }
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

void ParseBlockFile(const char *FilePath) {
  char *FileContent = LoadFileText(FilePath);
  if (FileContent == (void*)0) {
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: Failed to read file: %s", FilePath);
    return;
  }

  cJSON *Json = cJSON_Parse(FileContent);
  if (Json == (void*)0) {
    const char *ErrorPtr = cJSON_GetErrorPtr();
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: JSON syntax error %s before: %s",
             FilePath, ErrorPtr);
    UnloadFileText(FileContent);
    return;
  }

  cJSON *IdItem = cJSON_GetObjectItemCaseSensitive(Json, "id");
  if (cJSON_IsNumber(IdItem) == 0) {
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: FILE %s does not have a valid id",
             FilePath);
    cJSON_Delete(Json);
    UnloadFileText(FileContent);
    return;
  }

  int ParsedId = IdItem->valueint;
  if (ParsedId < 0 || ParsedId >= BLOCK_REGISTRY_SIZE) {
    TraceLog(
        LOG_WARNING,
        "BLOCK_SYSTEM: id %d from file %s is out of the allowed range (0-%d)",
        ParsedId, FilePath, BLOCK_REGISTRY_SIZE - 1);
    cJSON_Delete(Json);
    UnloadFileText(FileContent);
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

  TraceLog(LOG_INFO, "BLOCK_SYSTEM: Loaded [%d] %s", ParsedId, Block->Name);

  cJSON_Delete(Json);
  UnloadFileText(FileContent);
}

void LoadAllBlockDefinitions(const char *DirectoryPath) {
  const char *Path = TextFormat("./%s", DirectoryPath);

  TraceLog(LOG_INFO, "BLOCK_SYSTEM: Attempting to load from: %s%s",
           GetWorkingDirectory(), Path);

  if (!DirectoryExists(Path)) {
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: Directory not found %s", Path);
    return;
  }

  FilePathList Files = LoadDirectoryFiles(Path);
  int LoadedCount = 0;

  TraceLog(LOG_INFO, "BLOCK_SYSTEM: Initializing blocks in: %s", Path);

  int FilesCount = (int)Files.count;
  int TargetCount = FilesCount < MAX_FILES_LIMIT ? FilesCount : MAX_FILES_LIMIT;

  #pragma unroll 4
  for (int Idx = 0; Idx < MAX_FILES_LIMIT; Idx++) {
    if (Idx >= TargetCount) {
      break;
    }
    if (IsFileExtension(Files.paths[Idx], ".json")) {
      ParseBlockFile(Files.paths[Idx]);
      LoadedCount++;
    }
  }

  *GetLoadedBlocksCountPtr() = LoadedCount;
  UnloadDirectoryFiles(Files);

  if (LoadedCount == 0) {
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: No .json file found in %s", Path);
  } else {
    TraceLog(LOG_INFO, "BLOCK_SYSTEM: Loaded %d block definitions",
             LoadedCount);
  }
}

int GetLoadedBlocksCount(void) { return *GetLoadedBlocksCountPtr(); }
