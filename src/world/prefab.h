#ifndef PREFAB_H
#define PREFAB_H

#include "core/vecmath.h"
#include <stdbool.h>

struct World;
struct Chunk;

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
  // Optional stamp anchor, local to the prefab. Valid only when HasOrigin is
  // set; otherwise stamping uses the default horizontal-center / base placement.
  int OriginX;
  int OriginY;
  int OriginZ;
  PrefabCell *Cells;
  int CellCount;
  bool HasOrigin;
  // Orientation eligibility for the feature pass, both default true. When
  // AllowRotation is false the prefab is locked to its canonical Y-axis
  // rotation; when AllowMirror is false it is never horizontally mirrored.
  bool AllowRotation;
  bool AllowMirror;
} Prefab;

void InitPrefabRegistry(void);
void LoadAllPrefabs(const char *DirectoryPath);

// Parse, compile, and register a single prefab JSON file into the runtime
// registry. Replaces an existing prefab of the same name in place, otherwise
// appends a new entry. Returns true on success.
bool RegisterPrefabFile(const char *FilePath);
const Prefab *GetPrefab(const char *Name);
const Prefab *GetPrefabByIndex(int Index);

// Resolve a prefab name to its registry index, or -1 when the name is NULL or
// matches no loaded prefab. Lets data-driven tables (e.g. biome structure sets)
// store a stable index at load time and skip string compares during generation.
int GetPrefabIndexByName(const char *Name);

int GetLoadedPrefabsCount(void);

// Write each packed cell of Prefab into the world at Origin plus the cell's
// local offset, routing through SetBlockInWorld so affected chunks are marked
// dirty for remeshing.
void StampPrefab(struct World *World, const Prefab *Prefab, Vec3 Origin);

// Write Prefab into a single chunk's block data at the given world origin,
// clipped to that chunk: cells mapping outside the chunk are skipped, and a cell
// is written only over air, updating SolidBlockCount. The world origin is the
// stamp anchor — the prefab's declared origin is subtracted when set, otherwise
// the prefab is horizontally centered with its base at Y so the trunk lands on
// the chosen column. Unlike StampPrefab this never routes through the world or
// render layers, so it is safe on the chunk a worker thread already owns.
//
// Orientation is a 0-7 code applied to each cell's horizontal offset from the
// anchor column, leaving heights untouched: bits 0-1 select a Y-axis rotation
// (0/90/180/270 degrees) and bit 2 selects a horizontal mirror applied before
// the rotation. Code 0 is the canonical, untransformed placement. The anchor
// column stays fixed under any orientation, so the trunk lands on the chosen
// column and chunks re-deriving an overhanging structure stamp complementary
// fragments.
void StampPrefabIntoChunk(struct Chunk *ChunkVal, const Prefab *PrefabVal,
                          int WorldOriginX, int WorldOriginY, int WorldOriginZ,
                          int Orientation);

#endif
