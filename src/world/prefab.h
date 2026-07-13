#ifndef PREFAB_H
#define PREFAB_H

#include "core/vecmath.h"

struct World;

enum {
  MAX_PREFAB_NAME_SIZE = 32,
  MAX_PREFABS = 256
};

// One occupied block of a prefab. Positions are local to the prefab origin and
// the block id is already resolved from the palette at load time. Air cells are
// not stored (the format is sparse), so a prefab holds exactly CellCount of
// these. Byte fields keep the packed array tight; prefab axes stay well under
// 256 blocks.
typedef struct {
  unsigned char X;
  unsigned char Y;
  unsigned char Z;
  unsigned char BlockId;
} PrefabCell;

// Compiled, renderer-free prefab: dimensions plus a flat heap array of occupied
// cells. No palette strings or cJSON nodes are retained after load.
typedef struct {
  char Name[MAX_PREFAB_NAME_SIZE];
  int SizeX;
  int SizeY;
  int SizeZ;
  PrefabCell *Cells;
  int CellCount;
} Prefab;

void InitPrefabRegistry(void);
void LoadAllPrefabs(const char *DirectoryPath);
const Prefab *GetPrefab(const char *Name);
const Prefab *GetPrefabByIndex(int Index);
int GetLoadedPrefabsCount(void);

// Write each packed cell of Prefab into the world at Origin plus the cell's
// local offset, routing through SetBlockInWorld so affected chunks are marked
// dirty for remeshing.
void StampPrefab(struct World *World, const Prefab *Prefab, Vec3 Origin);

#endif
