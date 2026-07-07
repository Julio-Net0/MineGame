#ifndef CHUNK_H
#define CHUNK_H

#include "core/vecmath.h"
#include "core/mesh_handle.h"
#include <stdatomic.h>
#include <stdbool.h>

#define CHUNK_SIZE 16
#define CHUNK_VOLUME ((__SIZE_TYPE__)CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)
#define CHUNK_HALF_SIZE (CHUNK_SIZE / 2.0F)

#define BLOCK_SIZE 1.0F
#define BLOCK_HALF_SIZE (BLOCK_SIZE / 2.0F)

typedef struct Chunk {
  MeshHandle ChunkMesh;
  MeshHandle TranslucentMesh;

  unsigned char Data[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];

  int ChunkX;
  int ChunkY;
  int ChunkZ;
  int SolidBlockCount;

  atomic_bool IsDirty;
  bool HasMesh;
  bool HasTranslucentMesh;
  bool IsModified;

  atomic_bool IsGenerating;
  atomic_bool IsGenerated;
  atomic_bool TerrainJustGenerated;
} Chunk;

void GenerateChunkTerrain(Chunk *ChunkVal);
void SetBlockInChunk(Chunk *ChunkVal, Vec3 Pos, unsigned char BlockId);
int GetBlockIdInChunk(Chunk *ChunkVal, Vec3 Pos);

#endif
