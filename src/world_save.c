#include "world_save.h"
#include "raylib.h"
#include "chunk.h"
#include "chunk_serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

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

static int FloorDiv8(int n) {
  if (n < 0) {
    return (n - 7) / 8;
  }
  return n / 8;
}

static int LocalIndex(int chunkX, int chunkY, int chunkZ) {
  int rx = chunkX % 8; if (rx < 0) rx += 8;
  int ry = chunkY % 8; if (ry < 0) ry += 8;
  int rz = chunkZ % 8; if (rz < 0) rz += 8;
  return rx * 64 + ry * 8 + rz;
}

static FILE* OpenFileWithRetry(const char *path, const char *mode) {
  FILE *f = NULL;
  int retries = 5;
  while (retries-- > 0) {
    f = fopen(path, mode);
    if (f != NULL) {
      break;
    }
    if (errno == ENOENT) {
      break;
    }
    WaitTime(0.001);
  }
  return f;
}

void SaveChunkToDisk(Chunk *chunk) {
  uint8_t tempBuffer[4096];
  bool isRaw = false;
  int dataSize = ChunkSerialize(chunk, tempBuffer, &isRaw);

  int rx = FloorDiv8(chunk->chunkX);
  int ry = FloorDiv8(chunk->chunkY);
  int rz = FloorDiv8(chunk->chunkZ);

  char path[256];
  snprintf(path, sizeof(path), "world/r.%d.%d.%d.bin", rx, ry, rz);

  FILE *f = OpenFileWithRetry(path, "r+b");
  if (f == NULL) {
    f = OpenFileWithRetry(path, "w+b");
    if (f == NULL) {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to create region file %s", path);
      return;
    }
    uint8_t emptyHeader[1536] = {0};
    if (fwrite(emptyHeader, 1, sizeof(emptyHeader), f) != sizeof(emptyHeader)) {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write empty header to %s", path);
      fclose(f);
      return;
    }
  }

  int idx = LocalIndex(chunk->chunkX, chunk->chunkY, chunk->chunkZ);

  uint8_t flags = 1;
  if (isRaw) {
    flags |= 2;
  }

  uint8_t entry[3];
  entry[0] = flags;
  entry[1] = (uint8_t)(dataSize & 0xFF);
  entry[2] = (uint8_t)((dataSize >> 8) & 0xFF);

  if (fseek(f, idx * 3, SEEK_SET) != 0) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to seek header entry in %s", path);
    fclose(f);
    return;
  }
  if (fwrite(entry, 1, 3, f) != 3) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write header entry in %s", path);
    fclose(f);
    return;
  }

  long slotOffset = 1536 + (long)idx * 4096;
  if (fseek(f, slotOffset, SEEK_SET) != 0) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to seek body slot in %s", path);
    fclose(f);
    return;
  }
  if (fwrite(tempBuffer, 1, dataSize, f) != dataSize) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write body slot in %s", path);
    fclose(f);
    return;
  }

  fclose(f);
}

bool LoadChunkFromDisk(Chunk *chunk) {
  int rx = FloorDiv8(chunk->chunkX);
  int ry = FloorDiv8(chunk->chunkY);
  int rz = FloorDiv8(chunk->chunkZ);

  char path[256];
  snprintf(path, sizeof(path), "world/r.%d.%d.%d.bin", rx, ry, rz);

  FILE *f = OpenFileWithRetry(path, "rb");
  if (f == NULL) {
    return false;
  }

  int idx = LocalIndex(chunk->chunkX, chunk->chunkY, chunk->chunkZ);

  if (fseek(f, idx * 3, SEEK_SET) != 0) {
    fclose(f);
    return false;
  }

  uint8_t entry[3];
  if (fread(entry, 1, 3, f) != 3) {
    fclose(f);
    return false;
  }

  uint8_t flags = entry[0];
  uint16_t dataSize = entry[1] | (entry[2] << 8);

  if ((flags & 1) == 0) {
    fclose(f);
    return false;
  }

  long slotOffset = 1536 + (long)idx * 4096;
  if (fseek(f, slotOffset, SEEK_SET) != 0) {
    fclose(f);
    return false;
  }

  uint8_t tempBuffer[4096];
  if (fread(tempBuffer, 1, dataSize, f) != dataSize) {
    fclose(f);
    return false;
  }

  fclose(f);

  bool isRaw = (flags & 2) != 0;
  bool success = ChunkDeserialize(chunk, tempBuffer, dataSize, isRaw);
  if (success) {
    chunk->isModified = false;
  }
  return success;
}
