#ifndef WORLD_H
#define WORLD_H

#include "raylib.h"

#define CHUNK_SIZE 16

typedef struct {
  unsigned char data[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];
  Vector3 position;
} Chunk;

void GenerateFlatChunk(Chunk *chunk);

void DrawChunk(Chunk *chunk);

#endif
