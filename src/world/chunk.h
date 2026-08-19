#ifndef CHUNK_H
#define CHUNK_H

#include "core/vecmath.h"
#include "core/mesh_handle.h"
#include "world/biome.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define CHUNK_SIZE 16
#define CHUNK_VOLUME ((__SIZE_TYPE__)CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)
#define CHUNK_HALF_SIZE (CHUNK_SIZE / 2.0F)

#define BLOCK_SIZE 1.0F
#define BLOCK_HALF_SIZE (BLOCK_SIZE / 2.0F)

// Biomes are stored one id per BIOME_CELL_SIZE^3 block cell, not per block: 64
// bytes a chunk instead of 4096. Climate is a low-frequency field, so a cell's
// worth of blocks share a biome anyway, and the coarser grid also cuts the
// sampling generation has to do by the same factor.
#define BIOME_CELLS_PER_CHUNK (CHUNK_SIZE / BIOME_CELL_SIZE)

// Highest Y filled with water during generation. Shared so the feature pass can
// keep trees off submerged columns using the same threshold terrain uses.
enum {
  SEA_LEVEL = 18
};

typedef struct Chunk {
  MeshHandle ChunkMesh;

  // Alpha-cutout geometry (flora) kept apart from the solid faces. Both want
  // depth writes, so they shared one mesh; they are split because a fragment
  // shader containing `discard` costs the hardware early depth rejection for the
  // whole draw, and only this geometry needs the test.
  MeshHandle CutoutMesh;

  MeshHandle TranslucentMesh;

  unsigned char Data[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];

  // Derived from the seed, never serialized: FillChunkBiomeMap rebuilds it for
  // chunks loaded from disk exactly as it does for generated ones.
  unsigned char BiomeMap[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK]
                        [BIOME_CELLS_PER_CHUNK];

  int ChunkX;
  int ChunkY;
  int ChunkZ;
  int SolidBlockCount;

  atomic_bool IsDirty;
  bool HasMesh;
  bool HasCutoutMesh;
  bool HasTranslucentMesh;
  bool IsModified;

  atomic_bool IsGenerating;
  atomic_bool IsGenerated;
  atomic_bool TerrainJustGenerated;

  // Mesh work is outstanding for this chunk on a worker thread. Eviction must
  // not recycle the slot while it is set: the worker holds a snapshot, not a
  // pointer, so it is not a memory-safety fault, but the returning mesh would
  // describe a position the chunk no longer occupies.
  atomic_bool IsMeshing;

  // Set when the chunk is edited after its snapshot was captured. The mesh in
  // flight is then stale, so completion leaves the chunk dirty and it is queued
  // again rather than the edit staying invisible.
  atomic_bool DirtiedWhileMeshing;
} Chunk;

// Resolve the base blocks that are not part of any biome palette (stone, water)
// from the block registry by name. Must run after the blocks are loaded and
// before the chunk workers start.
void InitTerrainGeneration(void);

void GenerateChunkTerrain(Chunk *ChunkVal);

// Fill BiomeMap for this chunk's position. Call this for every chunk that
// becomes available for meshing, whether its blocks were generated or read back
// from disk — a loaded chunk has no biome map until this runs.
void FillChunkBiomeMap(Chunk *ChunkVal);

// Biome id of the cell containing a chunk-local block position.
unsigned char GetChunkBiomeAtLocal(const Chunk *ChunkVal, int LocalX,
                                   int LocalY, int LocalZ);

void SetBlockInChunk(Chunk *ChunkVal, Vec3 Pos, unsigned char BlockId);
int GetBlockIdInChunk(Chunk *ChunkVal, Vec3 Pos);

// Surface height of the terrain column at (GlobalX, GlobalZ) for a given seed.
// This is the single definition of the height field: generation, biome sampling
// (which derives its Depth axis from it), and debug commands all go through it,
// so none of them can drift out of sync with the terrain that actually exists.
int GetTerrainHeightAt(int GlobalX, int GlobalZ, uint64_t SeedVal);

#endif
