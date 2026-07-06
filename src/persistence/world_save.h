#ifndef WORLD_SAVE_H
#define WORLD_SAVE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Chunk Chunk;

void InitWorldSave(void);
void CloseWorldSave(void);
uint64_t GetWorldSeed(void);

void SaveChunkToDisk(Chunk *ChunkVal);
bool LoadChunkFromDisk(Chunk *ChunkVal);

#endif // WORLD_SAVE_H
