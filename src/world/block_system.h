#ifndef BLOCK_SYSTEM_H
#define BLOCK_SYSTEM_H

#include "core/tint.h"
#include <stdbool.h>

#define BLOCK_REGISTRY_SIZE 256
#define MAX_BLOCK_NAME_SIZE 32

// Sentinel for "this block has no side overlay". Negative so it can never
// collide with a real texture array layer, and so the shader can test for it.
#define NO_TEXTURE_OVERLAY (-1)

typedef struct {
  int TexTop;
  int TexSide;
  int TexBottom;
  int Id;

  // Optional second texture layer composited over the side faces, from
  // "texSideOverlay". Its alpha selects which texels belong to the overlay, and
  // only those take the biome tint — the base side texture never does. This is
  // what lets a grass block tint its grass fringe while the dirt under it keeps
  // its own colour. NO_TEXTURE_OVERLAY when the block declares none.
  int TexSideOverlay;

  char Name[MAX_BLOCK_NAME_SIZE];

  // Which biome palette this block's faces are multiplied by, from the optional
  // "tint" property. TINT_NONE leaves the block at its texture's own colors.
  TintSource Tint;

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
