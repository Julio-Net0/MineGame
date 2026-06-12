#include "world_save.h"
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define make_dir(path) mkdir(path, 0777)
#endif

typedef struct {
  char magic[4];
  uint32_t version;
  uint64_t seed;
} LevelMetadata;

static uint64_t g_worldSeed = 0;

static uint64_t generate_random_seed(void) {
  srand((unsigned int)time(NULL));
  uint64_t s1 = (uint64_t)rand() & 0xFFFF;
  uint64_t s2 = (uint64_t)rand() & 0xFFFF;
  uint64_t s3 = (uint64_t)rand() & 0xFFFF;
  uint64_t s4 = (uint64_t)rand() & 0xFFFF;
  return (s1 << 48) | (s2 << 32) | (s3 << 16) | s4;
}

void InitWorldSave(void) {
  // Create world directory if it doesn't exist
  make_dir("world");

  LevelMetadata meta;
  bool loaded = false;

  FILE *f = fopen("world/level.bin", "rb");
  if (f != NULL) {
    if (fread(&meta, sizeof(LevelMetadata), 1, f) == 1) {
      if (memcmp(meta.magic, "MGWL", 4) == 0) {
        g_worldSeed = meta.seed;
        loaded = true;
        TraceLog(LOG_INFO, "WORLD_SAVE: Loaded existing world seed: %llu", (unsigned long long)g_worldSeed);
      } else {
        TraceLog(LOG_WARNING, "WORLD_SAVE: Invalid magic in level.bin");
      }
    } else {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to read level.bin");
    }
    fclose(f);
  }

  if (!loaded) {
    g_worldSeed = generate_random_seed();
    TraceLog(LOG_INFO, "WORLD_SAVE: Generated new random world seed: %llu", (unsigned long long)g_worldSeed);

    f = fopen("world/level.bin", "wb");
    if (f != NULL) {
      memcpy(meta.magic, "MGWL", 4);
      meta.version = 1;
      meta.seed = g_worldSeed;
      if (fwrite(&meta, sizeof(LevelMetadata), 1, f) != 1) {
        TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write level.bin");
      }
      fclose(f);
    } else {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to create level.bin");
    }
  }
}

void CloseWorldSave(void) {
  // Currently nothing specific to cleanup for metadata, but placeholder for region files later
  TraceLog(LOG_INFO, "WORLD_SAVE: Closed save system.");
}

uint64_t GetWorldSeed(void) {
  return g_worldSeed;
}
