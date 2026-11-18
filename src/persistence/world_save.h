#ifndef WORLD_SAVE_H
#define WORLD_SAVE_H

#include <stdint.h>
#include <stdbool.h>

#define SCRIPT_DEFAULT_WORLD_SEED 16412814131703521744ULL

typedef struct Chunk Chunk;

void InitWorldSave(void);
void CloseWorldSave(void);
uint64_t GetWorldSeed(void);
void WorldSaveSetSeedOverride(uint64_t Seed);

// Read-only mode: every disk read and write path becomes a no-op, so a run that
// is only meant to observe the world (an automated script) neither alters what
// is on disk nor is contaminated by a local save. Must be set before
// InitWorldSave.
void WorldSaveSetReadOnly(bool ReadOnly);
bool WorldSaveIsReadOnly(void);

void SaveChunkToDisk(Chunk *ChunkVal);
bool LoadChunkFromDisk(Chunk *ChunkVal);

// Calls Visitor for every chunk position the save holds, read from the region
// headers alone. Does nothing in read-only mode. Meant for start-up, before the
// chunk workers read regions.
typedef void (*SavedChunkVisitor)(void *UserData, int ChunkX, int ChunkY,
                                  int ChunkZ);
void WorldSaveForEachSavedChunk(SavedChunkVisitor Visitor, void *UserData);

#endif // WORLD_SAVE_H
