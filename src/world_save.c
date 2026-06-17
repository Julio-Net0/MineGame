#include "world_save.h"
#include "raylib.h"
#include "chunk.h"
#include "chunk_serial.h"
#include <stdio.h>
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
  char Magic[4];
  uint32_t Version;
  uint64_t Seed;
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
  int64_t Key;
  pthread_mutex_t Mutex;
} RegionMutexEntry;

typedef struct {
  RegionMutexEntry RegionMutexTable[REGION_MUTEX_BUCKETS];
  int RegionMutexCount;
  pthread_mutex_t TableMutex;
  uint64_t WorldSeed;
} WorldSaveState;

static WorldSaveState *GetWorldSaveState(void) {
  static WorldSaveState State = {
    .RegionMutexCount = 0,
    .TableMutex = PTHREAD_MUTEX_INITIALIZER,
    .WorldSeed = 0
  };
  return &State;
}

static pthread_mutex_t* GetOrCreateRegionMutex(int Rx, int Ry, int Rz) {
  WorldSaveState *State = GetWorldSaveState();
  pthread_mutex_lock(&State->TableMutex);

  int64_t Key = ((int64_t)Rx << REGION_COORD_SHIFT_TOP) | ((int64_t)(Ry & REGION_COORD_MASK) << REGION_COORD_SHIFT_MID) | (int64_t)(Rz & REGION_COORD_MASK);
  if (Key == 0) { Key = INT64_MIN; }

  int Slot = (int)((uint64_t)Key % REGION_MUTEX_BUCKETS);
  #pragma unroll 4
  for (int IdxI = 0; IdxI < REGION_MUTEX_BUCKETS; IdxI++) {
    int Idx = (Slot + IdxI) % REGION_MUTEX_BUCKETS;
    if (State->RegionMutexTable[Idx].Key == Key) {
      pthread_mutex_unlock(&State->TableMutex);
      return &State->RegionMutexTable[Idx].Mutex;
    }
    if (State->RegionMutexTable[Idx].Key == 0) {
      if (State->RegionMutexCount >= REGION_HALF_FULL_LIMIT) {
        TraceLog(LOG_FATAL, "WORLD_SAVE: Region mutex table full, cannot lock region (%d,%d,%d)", Rx, Ry, Rz);
        pthread_mutex_unlock(&State->TableMutex);
        return (pthread_mutex_t *)0;
      }
      State->RegionMutexTable[Idx].Key = Key;
      pthread_mutex_init(&State->RegionMutexTable[Idx].Mutex, (const pthread_mutexattr_t *)0);
      State->RegionMutexCount++;
      pthread_mutex_unlock(&State->TableMutex);
      return &State->RegionMutexTable[Idx].Mutex;
    }
  }
  TraceLog(LOG_FATAL, "WORLD_SAVE: Region mutex table probe exhausted for region (%d,%d,%d)", Rx, Ry, Rz);
  pthread_mutex_unlock(&State->TableMutex);
  return (pthread_mutex_t *)0;
}

static uint64_t GenerateRandomSeed(void) {
#ifdef _WIN32
  uint64_t Hires = (uint64_t)__rdtsc();
#else
  struct timeval Tv;
  gettimeofday(&Tv, NULL);
  uint64_t Hires = (uint64_t)Tv.tv_sec * 1000000ULL + (uint64_t)Tv.tv_usec;
#endif
  return Hires ^ ((uint64_t)time((time_t *)0) * REGION_LCG_MULTIPLIER);
}

void InitWorldSave(void) {
  WorldSaveState *State = GetWorldSaveState();
  #pragma unroll 4
  for (int IdxI = 0; IdxI < REGION_MUTEX_BUCKETS; IdxI++) {
    State->RegionMutexTable[IdxI].Key = 0;
  }
  State->RegionMutexCount = 0;

  make_dir("world");

  LevelMetadata Meta;
  bool Loaded = false;

  FILE *FileVal = fopen("world/level.bin", "rb");
  if (FileVal != (FILE *)0) {
    if (fread(&Meta, sizeof(LevelMetadata), 1, FileVal) == 1) {
      bool MagicMatch = true;
      #pragma unroll 4
      for (int IdxI = 0; IdxI < 4; IdxI++) {
        if (Meta.Magic[IdxI] != "MGWL"[IdxI]) {
          MagicMatch = false;
        }
      }
      if (MagicMatch) {
        State->WorldSeed = Meta.Seed;
        Loaded = true;
        TraceLog(LOG_INFO, "WORLD_SAVE: Loaded existing world seed: %llu", (unsigned long long)State->WorldSeed);
      } else {
        TraceLog(LOG_WARNING, "WORLD_SAVE: Invalid magic in level.bin");
      }
    } else {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to read level.bin");
    }
    fclose(FileVal);
  }

  if (!Loaded) {
    State->WorldSeed = GenerateRandomSeed();
    TraceLog(LOG_INFO, "WORLD_SAVE: Generated new random world seed: %llu", (unsigned long long)State->WorldSeed);

    FileVal = fopen("world/level.bin", "wb");
    if (FileVal != (FILE *)0) {
      #pragma unroll 4
      for (int IdxI = 0; IdxI < 4; IdxI++) {
        Meta.Magic[IdxI] = "MGWL"[IdxI];
      }
      Meta.Version = 1;
      Meta.Seed = State->WorldSeed;
      if (fwrite(&Meta, sizeof(LevelMetadata), 1, FileVal) != 1) {
        TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write level.bin");
      }
      fclose(FileVal);
    } else {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to create level.bin");
    }
  }
}

void CloseWorldSave(void) {
  WorldSaveState *State = GetWorldSaveState();
  #pragma unroll 4
  for (int IdxI = 0; IdxI < REGION_MUTEX_BUCKETS; IdxI++) {
    if (State->RegionMutexTable[IdxI].Key != 0) {
      pthread_mutex_destroy(&State->RegionMutexTable[IdxI].Mutex);
    }
  }
  TraceLog(LOG_INFO, "WORLD_SAVE: Closed save system.");
}

uint64_t GetWorldSeed(void) {
  return GetWorldSaveState()->WorldSeed;
}

static int FloorDiv8(int N) {
  if (N < 0) {
    return (N - (REGION_AXIS_SIZE - 1)) / REGION_AXIS_SIZE;
  }
  return N / REGION_AXIS_SIZE;
}

static int LocalIndex(int ChunkX, int ChunkY, int ChunkZ) {
  int Rx = ChunkX % REGION_AXIS_SIZE; if (Rx < 0) { Rx += REGION_AXIS_SIZE; }
  int Ry = ChunkY % REGION_AXIS_SIZE; if (Ry < 0) { Ry += REGION_AXIS_SIZE; }
  int Rz = ChunkZ % REGION_AXIS_SIZE; if (Rz < 0) { Rz += REGION_AXIS_SIZE; }
  return (Rx * REGION_LOCAL_SLICE) + (Ry * REGION_AXIS_SIZE) + Rz;
}

static FILE* OpenFileWithRetry(const char *Path, const char *Mode) {
  FILE *FileVal = (FILE *)0;
  int Retries = REGION_OPEN_RETRIES;
  while (Retries-- > 0) {
    FileVal = fopen(Path, Mode);
    if (FileVal != (FILE *)0) {
      break;
    }
    if (errno == ENOENT) {
      break;
    }
    WaitTime(REGION_WAIT_ON_BUSY);
  }
  return FileVal;
}

void SaveChunkToDisk(Chunk *ChunkVal) {
  uint8_t TempBuffer[REGION_SLOT_SIZE];
  bool IsRaw = false;
  int DataSize = ChunkSerialize(ChunkVal, TempBuffer, &IsRaw);

  int Rx = FloorDiv8(ChunkVal->ChunkX);
  int Ry = FloorDiv8(ChunkVal->ChunkY);
  int Rz = FloorDiv8(ChunkVal->ChunkZ);

  pthread_mutex_t *RegionMutex = GetOrCreateRegionMutex(Rx, Ry, Rz);
  if (RegionMutex == (pthread_mutex_t *)0) { return; }
  pthread_mutex_lock(RegionMutex);

  char Path[REGION_PATH_BUF_SIZE];
  snprintf(Path, sizeof(Path), "world/r.%d.%d.%d.bin", Rx, Ry, Rz);

  FILE *FileVal = OpenFileWithRetry(Path, "r+b");
  if (FileVal == (FILE *)0) {
    FileVal = OpenFileWithRetry(Path, "w+b");
    if (FileVal == (FILE *)0) {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to create region file %s", Path);
      pthread_mutex_unlock(RegionMutex);
      return;
    }
    uint8_t EmptyHeader[REGION_HEADER_SIZE];
    #pragma unroll 4
    for (int IdxI = 0; IdxI < REGION_HEADER_SIZE; IdxI++) {
      EmptyHeader[IdxI] = 0;
    }
    if (fwrite(EmptyHeader, 1, sizeof(EmptyHeader), FileVal) != sizeof(EmptyHeader)) {
      TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write empty header to %s", Path);
      fclose(FileVal);
      pthread_mutex_unlock(RegionMutex);
      return;
    }
  }

  int Idx = LocalIndex(ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);

  uint8_t Flags = 1;
  if (IsRaw) {
    Flags |= 2;
  }

  uint8_t Entry[REGION_HEADER_ENTRY_BYTES];
  Entry[0] = Flags;
  Entry[1] = (uint8_t)(DataSize & BYTE_MASK);
  Entry[2] = (uint8_t)((DataSize >> BYTE_SHIFT) & BYTE_MASK);

  if (fseek(FileVal, Idx * REGION_HEADER_ENTRY_BYTES, SEEK_SET) != 0) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to seek header entry in %s", Path);
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return;
  }
  if (fwrite(Entry, 1, REGION_HEADER_ENTRY_BYTES, FileVal) != REGION_HEADER_ENTRY_BYTES) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write header entry in %s", Path);
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return;
  }

  long SlotOffset = REGION_HEADER_SIZE + ((long)Idx * REGION_SLOT_SIZE);
  if (fseek(FileVal, SlotOffset, SEEK_SET) != 0) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to seek body slot in %s", Path);
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return;
  }
  if (fwrite(TempBuffer, 1, DataSize, FileVal) != DataSize) {
    TraceLog(LOG_WARNING, "WORLD_SAVE: Failed to write body slot in %s", Path);
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return;
  }

  fclose(FileVal);
  pthread_mutex_unlock(RegionMutex);
}

bool LoadChunkFromDisk(Chunk *ChunkVal) {
  int Rx = FloorDiv8(ChunkVal->ChunkX);
  int Ry = FloorDiv8(ChunkVal->ChunkY);
  int Rz = FloorDiv8(ChunkVal->ChunkZ);

  pthread_mutex_t *RegionMutex = GetOrCreateRegionMutex(Rx, Ry, Rz);
  if (RegionMutex == (pthread_mutex_t *)0) { return false; }
  pthread_mutex_lock(RegionMutex);

  char Path[REGION_PATH_BUF_SIZE];
  snprintf(Path, sizeof(Path), "world/r.%d.%d.%d.bin", Rx, Ry, Rz);

  FILE *FileVal = OpenFileWithRetry(Path, "rb");
  if (FileVal == (FILE *)0) {
    pthread_mutex_unlock(RegionMutex);
    return false;
  }

  int Idx = LocalIndex(ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);

  if (fseek(FileVal, Idx * REGION_HEADER_ENTRY_BYTES, SEEK_SET) != 0) {
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return false;
  }

  uint8_t Entry[REGION_HEADER_ENTRY_BYTES];
  if (fread(Entry, 1, REGION_HEADER_ENTRY_BYTES, FileVal) != REGION_HEADER_ENTRY_BYTES) {
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return false;
  }

  uint8_t Flags = Entry[0];
  uint16_t DataSize = (uint16_t)(Entry[1] | ((uint16_t)Entry[2] << BYTE_SHIFT));

  if ((Flags & 1) == 0) {
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return false;
  }

  long SlotOffset = REGION_HEADER_SIZE + ((long)Idx * REGION_SLOT_SIZE);
  if (fseek(FileVal, SlotOffset, SEEK_SET) != 0) {
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return false;
  }

  uint8_t TempBuffer[REGION_SLOT_SIZE];
  if (fread(TempBuffer, 1, DataSize, FileVal) != DataSize) {
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return false;
  }

  fclose(FileVal);
  pthread_mutex_unlock(RegionMutex);

  bool IsRaw = (Flags & 2) != 0;
  bool Success = ChunkDeserialize(ChunkVal, TempBuffer, DataSize, IsRaw);
  if (Success) {
    ChunkVal->IsModified = false;
  }
  return Success;
}
