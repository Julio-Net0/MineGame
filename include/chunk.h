#ifndef CHUNK_H
#define CHUNK_H

#include "raylib.h"

typedef struct World World;

#define CHUNK_SIZE 16
#define CHUNK_VOLUME ((size_t)CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)
#define CHUNK_HALF_SIZE (CHUNK_SIZE / 2)

#define BLOCK_SIZE 1.0F
#define BLOCK_HALF_SIZE (BLOCK_SIZE / 2.0F)

#include <stdatomic.h>

typedef struct Chunk {
  Mesh mesh;
  Mesh translucentMesh;

  unsigned char data[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];

  int chunkX;
  int chunkY;
  int chunkZ;
  int solidBlockCount;

  atomic_bool isDirty;
  bool hasMesh;
  bool hasTranslucentMesh;
  bool isModified;

  atomic_bool isGenerating;
  atomic_bool isGenerated;
  atomic_bool terrainJustGenerated;
} Chunk;

void GenerateChunkTerrain(Chunk *chunk);

void SetBlockInChunk(Chunk *chunk, Vector3 pos, unsigned char blockID);
int GetBlockIDInChunk(Chunk *chunk, Vector3 pos);

#endif
