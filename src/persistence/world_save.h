#ifndef WORLD_SAVE_H
#define WORLD_SAVE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Chunk Chunk;

void InitWorldSave(void);
void CloseWorldSave(void);
uint64_t GetWorldSeed(void);

// Read-only mode: every write path becomes a no-op while reads keep working, so
// a run that is only meant to observe the world (an automated script) cannot
// alter what is on disk. Must be set before InitWorldSave, which would
// otherwise create the level metadata for a world that does not exist yet.
void WorldSaveSetReadOnly(bool ReadOnly);
bool WorldSaveIsReadOnly(void);

void SaveChunkToDisk(Chunk *ChunkVal);
bool LoadChunkFromDisk(Chunk *ChunkVal);

#endif // WORLD_SAVE_H
