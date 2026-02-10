#include "block_system.h"
#include "cJSON.h"
#include "raylib.h"
#include <string.h>

BlockType blockRegistry[BLOCK_REGISTRY_SIZE];

void InitBlockRegisry(void){
  for(int i = 0; i < BLOCK_REGISTRY_SIZE; i++){
    blockRegistry[i].id = -1;
    strcpy(blockRegistry[i].name, "");
    blockRegistry[i].color = (Color){0,0,0,0};
  }
}

BlockType* GetBlockDef(int id){
  if (id < 0 || id >= BLOCK_REGISTRY_SIZE){
    return &blockRegistry[0];
  }

  if(blockRegistry[id].id == -1){
    return &blockRegistry[0];
  }

  return &blockRegistry[id];
}

Color GetColorFromName(const char* name){ //REALLY TEMPORARY!!! Until I make blocks use textures
  if(strcmp(name, "GREEN") == 0) {return GREEN;} 
  if(strcmp(name, "GRAY") == 0) {return GRAY;} 

  return MAGENTA;
}

void ParseBlockFile(const char *filePath){
  char* fileContent = LoadFileText(filePath);
  if(fileContent == NULL){
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: Failed to read file: %s", filePath);
    return;
  }

  cJSON* json = cJSON_Parse(fileContent);
  if(json == NULL){
    const char* errorPtr = cJSON_GetErrorPtr();
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: JSON syntax error %s before: %s", filePath, errorPtr);
    UnloadFileText(fileContent);
    return;
  }

  cJSON* idItem = cJSON_GetObjectItemCaseSensitive(json, "id");
  if(!cJSON_IsNumber(idItem)){
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: FILE %s does not have a valid id", filePath);
    cJSON_Delete(json);
    UnloadFileText(fileContent);
    return;
  }

  int id = idItem->valueint;
  if(id <= 0 || id >= BLOCK_REGISTRY_SIZE){
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: id %d from file %s is out of the allowed range (1-%d)", id, filePath, BLOCK_REGISTRY_SIZE - 1);
    cJSON_Delete(json);
    UnloadFileText(fileContent);
    return;
  }

  BlockType* block = &blockRegistry[id];
  block->id = id;

  cJSON* nameItem = cJSON_GetObjectItemCaseSensitive(json, "name");
  if(cJSON_IsString(nameItem)){
    TextCopy(block->name, nameItem->valuestring);
  }

  cJSON* color = cJSON_GetObjectItemCaseSensitive(json, "color");
  if(cJSON_IsString(color)){
    block->color = GetColorFromName(color->valuestring);
  }

  TraceLog(LOG_INFO, "BLOCK_SYSTEM: Loaded [%d] %s", id, block->name);

  cJSON_Delete(json);
  UnloadFileText(fileContent);
}

void LoadAllBlockDefinitions(const char *directoryPath){

  const char* path = TextFormat("./%s", directoryPath);

  TraceLog(LOG_INFO, "BLOCK_SYSTEM: Attempting to load from: %s%s", GetWorkingDirectory(), path);

  if(!DirectoryExists(path)) {
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: Directory not found %s", path);
    return;
  }

  FilePathList files = LoadDirectoryFiles(path);

  int loadedCount = 0;

  TraceLog(LOG_INFO, "BLOCK_SYSTEM: Initializing blocks in: %s", path);

  for(int i = 0; i < files.count; i++){
    if(IsFileExtension(files.paths[i], ".json")){
      ParseBlockFile(files.paths[i]);
      loadedCount++;
    }
  }

  UnloadDirectoryFiles(files);

  if(loadedCount == 0){
    TraceLog(LOG_WARNING, "BLOCK_SYSTEM: No .json file found in %s", path);
  }else{
    TraceLog(LOG_INFO, "BLOCK_SYSTEM: Loaded %d block definitions", loadedCount);
  }
}
