#ifndef BLOCK_SYSTEM_H
#define BLOCK_SYSTEM_H

#include "raylib.h"
#include <stdbool.h>

#define BLOCK_REGISTRY_SIZE 256
#define MAX_BLOCK_NAME_SIZE 32

typedef struct {
  int id;
  char name[MAX_BLOCK_NAME_SIZE];
  Color color;
} BlockType;

extern BlockType blockRegistry[BLOCK_REGISTRY_SIZE];

void InitBlockRegisry(void);
void LoadAllBlockDefinitions(const char* directoryPath);
Color GetColorFromName(const char* name);

void ParseBlockFile(const char* filePath);
bool IsValidBlockFile(const char* fileName);

BlockType* GetBlockDef(int id);
int GetBlockIDByName(const char* name);

#endif
