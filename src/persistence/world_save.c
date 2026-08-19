#include "persistence/world_save.h"
#include "core/log.h"
#include "world/chunk.h"
#include "persistence/chunk_serial.h"
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>

#define BYTE_MASK 0xFF
#define BYTE_SHIFT 8

#define MILLIS_PER_SECOND 1000U
#define NANOS_PER_MILLI   1000000L
#define MICROS_PER_SECOND 1000000ULL

#ifdef _WIN32
#include <direct.h>
#include <intrin.h>
#include <windows.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#define make_dir(path) mkdir(path, 0777)
#endif

// Renderer-free millisecond sleep, replacing Raylib's WaitTime for the
// region-file open retry back-off.
static void SleepMillis(unsigned Millis) {
#ifdef _WIN32
  Sleep(Millis);
#else
  struct timespec Req = {.tv_sec = Millis / MILLIS_PER_SECOND,
                         .tv_nsec = (long)(Millis % MILLIS_PER_SECOND) * NANOS_PER_MILLI};
  nanosleep(&Req, (struct timespec *)0);
#endif
}

typedef struct {
  char Magic[4];
  uint32_t Version;
  uint64_t Seed;
} LevelMetadata;

// Region file locks are striped, not keyed: a fixed set of mutexes, all
// initialised at startup, chosen by hashing the region coordinate. Two regions
// may share a stripe, which only means their file operations serialise against
// each other — the work under the lock is disk I/O either way.
//
// This count is not a capacity. It trades memory against collision probability
// and nothing else; five threads contend (four workers plus the main thread
// saving during eviction), so 64 makes an incidental collision rare. Raising or
// lowering it cannot break correctness.
#define REGION_MUTEX_STRIPES 64

#define REGION_AXIS_SIZE       8
#define REGION_LOCAL_SLICE     (REGION_AXIS_SIZE * REGION_AXIS_SIZE)
#define REGION_TOTAL_CHUNKS    (REGION_AXIS_SIZE * REGION_AXIS_SIZE * REGION_AXIS_SIZE)
#define REGION_HEADER_ENTRY_BYTES 3
// Long-typed so the header offsets it feeds are computed in the width fseek
// takes, rather than multiplied as int and widened afterwards.
#define REGION_HEADER_SIZE     ((long)REGION_TOTAL_CHUNKS * REGION_HEADER_ENTRY_BYTES)
#define REGION_SLOT_SIZE       4096
#define REGION_PATH_BUF_SIZE   256
#define REGION_COORD_BITS      20
#define REGION_COORD_MASK      0xFFFFF
#define REGION_COORD_SHIFT_MID 20
#define REGION_COORD_SHIFT_TOP 40
#define REGION_LCG_MULTIPLIER  6364136223846793005ULL
#define REGION_OPEN_RETRIES    5
#define REGION_WAIT_ON_BUSY_MS 1

// SplitMix64 finalizer constants, the same construction world/feature.c uses for
// its cell hashing.
#define REGION_MIX_A 0xBF58476D1CE4E5B9ULL
#define REGION_MIX_B 0x94D049BB133111EBULL
#define REGION_MIX_SHIFT_A 30U
#define REGION_MIX_SHIFT_B 27U
#define REGION_MIX_SHIFT_C 31U

typedef struct {
  pthread_mutex_t RegionMutexes[REGION_MUTEX_STRIPES];
  uint64_t WorldSeed;
} WorldSaveState;

static WorldSaveState *GetWorldSaveState(void) {
  static WorldSaveState State = { .WorldSeed = 0 };
  return &State;
}

// All three axes are mixed, not truncated. The previous derivation packed the
// coordinate as Rx<<40 | Ry<<20 | Rz and took it modulo the bucket count, which
// kept only the low bits — those come entirely from Rz, so Rx and Ry never
// affected the slot and sixteen regions differing only in those two axes landed
// on one bucket.
static int RegionStripeIndex(int Rx, int Ry, int Rz) {
  uint64_t Hash = ((uint64_t)(uint32_t)Rx << REGION_COORD_SHIFT_TOP) ^
                  ((uint64_t)(uint32_t)Ry << REGION_COORD_SHIFT_MID) ^
                  (uint64_t)(uint32_t)Rz;

  Hash = (Hash ^ (Hash >> REGION_MIX_SHIFT_A)) * REGION_MIX_A;
  Hash = (Hash ^ (Hash >> REGION_MIX_SHIFT_B)) * REGION_MIX_B;
  Hash = Hash ^ (Hash >> REGION_MIX_SHIFT_C);

  // Unsigned throughout, so a negative region coordinate cannot produce a
  // negative index.
  return (int)(Hash % (uint64_t)REGION_MUTEX_STRIPES);
}

// Always returns a usable lock. Every mutex is initialised before any worker
// thread exists, so this reads no shared mutable state and can neither fail nor
// need a lock of its own.
static pthread_mutex_t *GetRegionMutex(int Rx, int Ry, int Rz) {
  WorldSaveState *State = GetWorldSaveState();
  return &State->RegionMutexes[RegionStripeIndex(Rx, Ry, Rz)];
}

static uint64_t GenerateRandomSeed(void) {
#ifdef _WIN32
  uint64_t Hires = (uint64_t)__rdtsc();
#else
  struct timeval Tv;
  gettimeofday(&Tv, NULL);
  uint64_t Hires = ((uint64_t)Tv.tv_sec * MICROS_PER_SECOND) + (uint64_t)Tv.tv_usec;
#endif
  return Hires ^ ((uint64_t)time((time_t *)0) * REGION_LCG_MULTIPLIER);
}

void InitWorldSave(void) {
  WorldSaveState *State = GetWorldSaveState();
  // Before InitChunkWorker creates any thread, so no lookup ever initialises.
  for (int IdxI = 0; IdxI < REGION_MUTEX_STRIPES; IdxI++) {
    pthread_mutex_init(&State->RegionMutexes[IdxI],
                       (const pthread_mutexattr_t *)0);
  }

  make_dir("world");

  LevelMetadata Meta;
  bool Loaded = false;

  FILE *FileVal = fopen("world/level.bin", "rb");
  if (FileVal != (FILE *)0) {
    if (fread(&Meta, sizeof(LevelMetadata), 1, FileVal) == 1) {
      bool MagicMatch = true;
      for (int IdxI = 0; IdxI < 4; IdxI++) {
        if (Meta.Magic[IdxI] != "MGWL"[IdxI]) {
          MagicMatch = false;
        }
      }
      if (MagicMatch) {
        State->WorldSeed = Meta.Seed;
        Loaded = true;
        LogInfo("WORLD_SAVE: Loaded existing world seed: %llu", (unsigned long long)State->WorldSeed);
      } else {
        LogWarn("WORLD_SAVE: Invalid magic in level.bin");
      }
    } else {
      LogWarn("WORLD_SAVE: Failed to read level.bin");
    }
    fclose(FileVal);
  }

  if (!Loaded) {
    State->WorldSeed = GenerateRandomSeed();
    LogInfo("WORLD_SAVE: Generated new random world seed: %llu", (unsigned long long)State->WorldSeed);

    FileVal = fopen("world/level.bin", "wb");
    if (FileVal != (FILE *)0) {
      for (int IdxI = 0; IdxI < 4; IdxI++) {
        Meta.Magic[IdxI] = "MGWL"[IdxI];
      }
      Meta.Version = 1;
      Meta.Seed = State->WorldSeed;
      if (fwrite(&Meta, sizeof(LevelMetadata), 1, FileVal) != 1) {
        LogWarn("WORLD_SAVE: Failed to write level.bin");
      }
      fclose(FileVal);
    } else {
      LogWarn("WORLD_SAVE: Failed to create level.bin");
    }
  }
}

void CloseWorldSave(void) {
  WorldSaveState *State = GetWorldSaveState();
  for (int IdxI = 0; IdxI < REGION_MUTEX_STRIPES; IdxI++) {
    pthread_mutex_destroy(&State->RegionMutexes[IdxI]);
  }
  LogInfo("WORLD_SAVE: Closed save system.");
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
    SleepMillis(REGION_WAIT_ON_BUSY_MS);
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

  pthread_mutex_t *RegionMutex = GetRegionMutex(Rx, Ry, Rz);
  pthread_mutex_lock(RegionMutex);

  char Path[REGION_PATH_BUF_SIZE];
  snprintf(Path, sizeof(Path), "world/r.%d.%d.%d.bin", Rx, Ry, Rz);

  FILE *FileVal = OpenFileWithRetry(Path, "r+b");
  if (FileVal == (FILE *)0) {
    FileVal = OpenFileWithRetry(Path, "w+b");
    if (FileVal == (FILE *)0) {
      LogWarn("WORLD_SAVE: Failed to create region file %s", Path);
      pthread_mutex_unlock(RegionMutex);
      return;
    }
    uint8_t EmptyHeader[REGION_HEADER_SIZE];
    for (int IdxI = 0; IdxI < REGION_HEADER_SIZE; IdxI++) {
      EmptyHeader[IdxI] = 0;
    }
    if (fwrite(EmptyHeader, 1, sizeof(EmptyHeader), FileVal) != sizeof(EmptyHeader)) {
      LogWarn("WORLD_SAVE: Failed to write empty header to %s", Path);
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

  if (fseek(FileVal, (long)Idx * REGION_HEADER_ENTRY_BYTES, SEEK_SET) != 0) {
    LogWarn("WORLD_SAVE: Failed to seek header entry in %s", Path);
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return;
  }
  if (fwrite(Entry, 1, REGION_HEADER_ENTRY_BYTES, FileVal) != REGION_HEADER_ENTRY_BYTES) {
    LogWarn("WORLD_SAVE: Failed to write header entry in %s", Path);
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return;
  }

  long SlotOffset = REGION_HEADER_SIZE + ((long)Idx * REGION_SLOT_SIZE);
  if (fseek(FileVal, SlotOffset, SEEK_SET) != 0) {
    LogWarn("WORLD_SAVE: Failed to seek body slot in %s", Path);
    fclose(FileVal);
    pthread_mutex_unlock(RegionMutex);
    return;
  }
  if (fwrite(TempBuffer, 1, DataSize, FileVal) != DataSize) {
    LogWarn("WORLD_SAVE: Failed to write body slot in %s", Path);
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

  pthread_mutex_t *RegionMutex = GetRegionMutex(Rx, Ry, Rz);
  pthread_mutex_lock(RegionMutex);

  char Path[REGION_PATH_BUF_SIZE];
  snprintf(Path, sizeof(Path), "world/r.%d.%d.%d.bin", Rx, Ry, Rz);

  FILE *FileVal = OpenFileWithRetry(Path, "rb");
  if (FileVal == (FILE *)0) {
    pthread_mutex_unlock(RegionMutex);
    return false;
  }

  int Idx = LocalIndex(ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);

  if (fseek(FileVal, (long)Idx * REGION_HEADER_ENTRY_BYTES, SEEK_SET) != 0) {
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
