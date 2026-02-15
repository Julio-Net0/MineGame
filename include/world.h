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

typedef struct {
  bool hit;
  Vector3 blockPos;
  Vector3 normal;
  int blockID;
} RaycastResult;

RaycastResult RayCastToBlock(Chunk *chunk, Vector3 rayOrigin, Vector3 rayDir, float maxDistance);

void SetBlock(Chunk* chunk, Vector3 pos, unsigned char blockID);

#endif
