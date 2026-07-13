#ifndef BLOCK_SYSTEM_H
#define BLOCK_SYSTEM_H

#include <stdbool.h>

#define BLOCK_REGISTRY_SIZE 256
#define MAX_BLOCK_NAME_SIZE 32

typedef struct {
  int TexTop;
  int TexSide;
  int TexBottom;
  int Id;

  char Name[MAX_BLOCK_NAME_SIZE];

  bool IsTransparent;
  bool IsSolid;
} BlockType;

BlockType *GetBlockRegistry(void);

void InitBlockRegistry(void);
void LoadAllBlockDefinitions(const char *DirectoryPath);
void ParseBlockFile(const char *FilePath);
BlockType *GetBlockDef(int Id);
int GetBlockIdByName(const char *Name);
int GetLoadedBlocksCount(void);

#endif
