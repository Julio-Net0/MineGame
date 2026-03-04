#ifndef BLOCK_SYSTEM_H
#define BLOCK_SYSTEM_H

#include "raylib.h"
#include <stdbool.h>

#define BLOCK_REGISTRY_SIZE 256
#define MAX_BLOCK_NAME_SIZE 32

typedef struct {
  int texTop;
  int texSide;
  int texBottom;
  int id;

  char name[MAX_BLOCK_NAME_SIZE];

  bool isTransparent;
} BlockType;

extern BlockType blockRegistry[BLOCK_REGISTRY_SIZE];

void InitBlockRegistry(void);
void LoadAllBlockDefinitions(const char* directoryPath);
void ParseBlockFile(const char* filePath);
BlockType* GetBlockDef(int id);
int GetLoadedBlocksCount(void);

#endif
