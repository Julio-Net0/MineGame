#ifndef WORLD_STONE_FILLER_H
#define WORLD_STONE_FILLER_H

#include "world/block_id.h"
#include <stdbool.h>
#include <stdint.h>

// The block below the biome palette layers, by depth: an ordered list of filler
// layers (stone, then deepslate) whose boundaries are hashed transition bands,
// plus stone pockets (tuff, granite, ...) placed as hashed ellipsoids, one at
// most per cubic pocket cell. Everything is a pure function of position and
// seed, so cubes agree across their boundaries and the per-block and SOLID
// paths give the same blocks.
//
// Tuning lives in assets/worldgen/filler.json. Written during startup only;
// read-only and lock-free afterwards.

enum {
  FILLER_MAX_LAYERS = 8,
  FILLER_MAX_POCKET_KINDS = 16,
  // Pockets whose bounding box can reach one cube: at most one per pocket cell
  // of the 3 x 3 x 3 cells around it.
  FILLER_MAX_CUBE_POCKETS = 27,
  FILLER_ROWS = 16
};

typedef struct {
  int CenterX;
  int CenterY;
  int CenterZ;
  int RadiusX;
  int RadiusY;
  int RadiusZ;
  int MinY;
  int MaxY;
  BlockID Block;
} FillerPocket;

// Everything a cube needs to decide its filler blocks, built once per cube.
typedef struct {
  int BaseY;
  uint64_t Seed;
  // Per row: the layer block, or 0 when the row lies in a transition band and
  // each block hashes between the two layers.
  BlockID RowBlock[FILLER_ROWS];
  // For a band row, the index of the layer below the band; -1 otherwise.
  int RowBand[FILLER_ROWS];
  int PocketCount;
  FillerPocket Pockets[FILLER_MAX_CUBE_POCKETS];
} FillerCube;

void InitStoneFiller(void);

// filler.json; block names resolve against the loaded block registry. A
// rejected file falls back to stone only, with the reason logged.
void LoadStoneFiller(const char *FilePath);

void FillerCubeInit(FillerCube *Cube, int ChunkX, int ChunkY, int ChunkZ,
                    uint64_t Seed);

// True, with the block, when every block of the cube is the same filler: no
// row in a transition band and no pocket reaching the cube.
bool FillerCubeUniform(const FillerCube *Cube, BlockID *OutBlock);

// The filler block at a position of the cube (global coordinates).
BlockID FillerBlockAt(const FillerCube *Cube, int X, int Y, int Z);

// Whether BlockId is one of the pocket kinds' blocks (preview statistics).
bool FillerIsPocketBlock(BlockID BlockId);

#endif
