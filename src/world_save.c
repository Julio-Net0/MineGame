#include "world_save.h"
#include "raylib.h"
#include "chunk.h"
#include "chunk_serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>

#define BYTE_MASK 0xFF
#define BYTE_SHIFT 8

#ifdef _WIN32
#include <direct.h>
#include <intrin.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#define make_dir(path) mkdir(path, 0777)
#endif

typedef struct {
  char magic[4];
  uint32_t version;
  uint64_t seed;
} LevelMetadata;

#define REGION_MUTEX_BUCKETS 64

#define REGION_AXIS_SIZE       8
#define REGION_LOCAL_SLICE     (REGION_AXIS_SIZE * REGION_AXIS_SIZE)
#define REGION_TOTAL_CHUNKS    (REGION_AXIS_SIZE * REGION_AXIS_SIZE * REGION_AXIS_SIZE)
#define REGION_HEADER_ENTRY_BYTES 3
#define REGION_HEADER_SIZE     (REGION_TOTAL_CHUNKS * REGION_HEADER_ENTRY_BYTES)
#define REGION_SLOT_SIZE       4096
#define REGION_PATH_BUF_SIZE   256
#define REGION_COORD_BITS      20
#define REGION_COORD_MASK      0xFFFFF
#define REGION_COORD_SHIFT_MID 20
#define REGION_COORD_SHIFT_TOP 40
#define REGION_LCG_MULTIPLIER  6364136223846793005ULL
#define REGION_OPEN_RETRIES    5
#define REGION_WAIT_ON_BUSY    0.001
#define REGION_HALF_FULL_LIMIT (REGION_MUTEX_BUCKETS / 2)

typedef struct {
  int64_t key;
  pthread_mutex_t mutex;
} RegionMutexEntry;

static RegionMutexEntry g_regionMutexTable[REGION_MUTEX_BUCKETS];
static int g_regionMutexCount = 0;
static pthread_mutex_t g_tableMutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t* GetOrCreateRegionMutex(int rx, int ry, int rz) {
  pthread_mutex_lock(&g_tableMutex);

  int64_t key = ((int64_t)rx << REGION_COORD_SHIFT_TOP) | ((int64_t)(ry & REGION_COORD_MASK) << REGION_COORD_SHIFT_MID) | (int64_t)(rz & REGION_COORD_MASK);
  if (key == 0) { key = INT64_MIN; }

  int slot = (int)((uint64_t)key % REGION_MUTEX_BUCKETS);
  for (int i = 0; i < REGION_MUTEX_BUCKETS; i++) {
    int idx = (slot + i) % REGION_MUTEX_BUCKETS;
    if (g_regionMutexTable[idx].key == key) {
      pthread_mutex_unlock(&g_tableMutex);
      return &g_regionMutexTable[idx].mutex;
    }
    if (g_regionMutexTable[idx].key == 0) {
      if (g_regionMutexCount >= REGION_HALF_FULL_LIMIT) {
        TraceLog(LOG_FATAL, "WORLD_SAVE: Region mutex table full, cannot lock region (%d,%d,%d)", rx, ry, rz);
        pthread_mutex_unlock(&g_tableMutex);
        return NULL;
      }
      g_regionMutexTable[idx].key = key;
      pthread_mutex_init(&g_regionMutexTable[idx].mutex, NULL);
      g_regionMutexCount++;
      pthread_mutex_unlock(&g_tableMutex);
      return &g_regionMutexTable[idx].mutex;
    }
  }
  TraceLog(LOG_FATAL, "WORLD_SAVE: Region mutex table probe exhausted for region (%d,%d,%d)", rx, ry, rz);
  pthread_mutex_unlock(&g_tableMutex);
  return NULL;
}

static uint64_t g_worldSeed = 0;

static uint64_t GenerateRandomSeed(void) {
#ifdef _WIN32
  uint64_t hires = (uint64_t)__rdtsc();
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint64_t hires = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
#endif
  return hires ^ ((uint64_t)time(NULL) * REGION_LCG_MULTIPLIER);
}

void InitWorldSave(void) {
  memset(g_regionMutexTable, 0, sizeof(g_regionMutexTable));
  g_regionMutexCount = 0;

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
    g_worldSeed = GenerateRandomSeed();
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
  for (int i = 0; i < REGION_MUTEX_BUCKETS; i++) {
    if (g_regionMutexTable[i].key != 0) {
      pthread_mutex_destroy(&g_regionMutexTable[i].mutex);
    }
  }
  TraceLog(LOG_INFO, "WORLD_SAVE: Closed save system.");
}

uint64_t GetWorldSeed(void) {
  return g_worldSeed;
}

static int FloorDiv8(int n) {
  if (n < 0) {
    return (n - (REGION_AXIS_SIZE - 1)) / REGION_AXIS_SIZE;
  }
  return n / REGION_AXIS_SIZE;
}

static int LocalIndex(int chunkX, int chunkY, int chunkZ) {
  int rx = chunkX % REGION_AXIS_SIZE; if (rx < 0) { rx += REGION_AXIS_SIZE; }
  int ry = chunkY % REGION_AXIS_SIZE; if (ry < 0) { ry += REGION_AXIS_SIZE; }
  int rz = chunkZ % REGION_AXIS_SIZE; if (rz < 0) { rz += REGION_AXIS_SIZE; }
  return (rx * REGION_LOCAL_SLICE) + (ry * REGION_AXIS_SIZE) + rz;
}

static FILE* OpenFileWithRetry(const char *path, const char *mode) {
  FILE *f = NULL;
  int retries = REGION_OPEN_RETRIES;
  while (retries-- > 0) {
    f = fopen(path, mode);
    if (f != NULL) {
      break;
    }
    if (errno == ENOENT) {
      break;
    }
    WaitTime(REGION_WAIT_ON_BUSY);
  }
  return f;
}

void SaveChunkToDisk(Chunk *chunk) {
  uint8_t tempBuffer[REGION_SLOT_SIZE];
  bool isRaw = false;
  int dataSize = ChunkSerialize(chunk, tempBuffer, &isRaw);

  int rx = FloorDiv8(chunk->chunkX);
  int ry = FloorDiv8(chunk->chunkY);
  int rz = FloorDiv8(chunk->chunkZ);

  pthread_mutex_t *regionMutex = GetOrCreateRegionMutex(rx, ry, rz);
  if (regionMutex == NULL) { return; }
  pthread_mutex_lock(regionMutex);

  char path[REGION_PATH_BUF_SIZE];
  snprintf(path, sizeof(path), "world/r.%d.%d.%d.bin", rx, ry, rz);

  FILE *f = OpenFileWithRetry(path, "r+b");
  if (f == NULL) {
    f = OpenFileWithRetry(path, "w+b");
    if (f == NULL) {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to create region file %s", path);
      pthread_mutex_unlock(regionMutex);
      return;
    }
    uint8_t emptyHeader[REGION_HEADER_SIZE] = {0};
    if (fwrite(emptyHeader, 1, sizeof(emptyHeader), f) != sizeof(emptyHeader)) {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write empty header to %s", path);
      fclose(f);
      pthread_mutex_unlock(regionMutex);
      return;
    }
  }

  int idx = LocalIndex(chunk->chunkX, chunk->chunkY, chunk->chunkZ);

  uint8_t flags = 1;
  if (isRaw) {
    flags |= 2;
  }

  uint8_t entry[REGION_HEADER_ENTRY_BYTES];
  entry[0] = flags;
  entry[1] = (uint8_t)(dataSize & BYTE_MASK);
  entry[2] = (uint8_t)((dataSize >> BYTE_SHIFT) & BYTE_MASK);

  if (fseek(f, idx * REGION_HEADER_ENTRY_BYTES, SEEK_SET) != 0) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to seek header entry in %s", path);
    fclose(f);
    pthread_mutex_unlock(regionMutex);
    return;
  }
  if (fwrite(entry, 1, REGION_HEADER_ENTRY_BYTES, f) != REGION_HEADER_ENTRY_BYTES) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write header entry in %s", path);
    fclose(f);
    pthread_mutex_unlock(regionMutex);
    return;
  }

  long slotOffset = REGION_HEADER_SIZE + ((long)idx * REGION_SLOT_SIZE);
  if (fseek(f, slotOffset, SEEK_SET) != 0) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to seek body slot in %s", path);
    fclose(f);
    pthread_mutex_unlock(regionMutex);
    return;
  }
  if (fwrite(tempBuffer, 1, dataSize, f) != dataSize) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write body slot in %s", path);
    fclose(f);
    pthread_mutex_unlock(regionMutex);
    return;
  }

  fclose(f);
  pthread_mutex_unlock(regionMutex);
}

bool LoadChunkFromDisk(Chunk *chunk) {
  int rx = FloorDiv8(chunk->chunkX);
  int ry = FloorDiv8(chunk->chunkY);
  int rz = FloorDiv8(chunk->chunkZ);

  pthread_mutex_t *regionMutex = GetOrCreateRegionMutex(rx, ry, rz);
  if (regionMutex == NULL) { return false; }
  pthread_mutex_lock(regionMutex);

  char path[REGION_PATH_BUF_SIZE];
  snprintf(path, sizeof(path), "world/r.%d.%d.%d.bin", rx, ry, rz);

  FILE *f = OpenFileWithRetry(path, "rb");
  if (f == NULL) {
    pthread_mutex_unlock(regionMutex);
    return false;
  }

  int idx = LocalIndex(chunk->chunkX, chunk->chunkY, chunk->chunkZ);

  if (fseek(f, idx * REGION_HEADER_ENTRY_BYTES, SEEK_SET) != 0) {
    fclose(f);
    pthread_mutex_unlock(regionMutex);
    return false;
  }

  uint8_t entry[REGION_HEADER_ENTRY_BYTES];
  if (fread(entry, 1, REGION_HEADER_ENTRY_BYTES, f) != REGION_HEADER_ENTRY_BYTES) {
    fclose(f);
    pthread_mutex_unlock(regionMutex);
    return false;
  }

  uint8_t flags = entry[0];
  uint16_t dataSize = (uint16_t)(entry[1] | ((uint16_t)entry[2] << BYTE_SHIFT));

  if ((flags & 1) == 0) {
    fclose(f);
    pthread_mutex_unlock(regionMutex);
    return false;
  }

  long slotOffset = REGION_HEADER_SIZE + ((long)idx * REGION_SLOT_SIZE);
  if (fseek(f, slotOffset, SEEK_SET) != 0) {
    fclose(f);
    pthread_mutex_unlock(regionMutex);
    return false;
  }

  uint8_t tempBuffer[REGION_SLOT_SIZE];
  if (fread(tempBuffer, 1, dataSize, f) != dataSize) {
    fclose(f);
    pthread_mutex_unlock(regionMutex);
    return false;
  }

  fclose(f);
  pthread_mutex_unlock(regionMutex);

  bool isRaw = (flags & 2) != 0;
  bool success = ChunkDeserialize(chunk, tempBuffer, dataSize, isRaw);
  if (success) {
    chunk->isModified = false;
  }
  return success;
}
