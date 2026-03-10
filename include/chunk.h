#ifndef CHUNK_H
#define CHUNK_H

#include "raylib.h"

typedef struct World World;

#define CHUNK_SIZE 16
#define CHUNK_HALF_SIZE (CHUNK_SIZE / 2)

#define BLOCK_SIZE 1.0F
#define BLOCK_HALF_SIZE (BLOCK_SIZE / 2.0F)

typedef struct {
  Mesh mesh;

  unsigned char data[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];

  Vector3 worldPosition;

  int chunkX;
  int chunkY;
  int chunkZ;
  int solidBlockCount;

  bool isDirty;
  bool hasMesh;
} Chunk;

void GenerateFlatChunk(Chunk *chunk);
void DrawChunk(World *world, Chunk *chunk);

void SetBlockInChunk(Chunk* chunk, Vector3 pos, unsigned char blockID);
int GetBlockIDInChunk(Chunk *chunk, Vector3 pos);

#endif
