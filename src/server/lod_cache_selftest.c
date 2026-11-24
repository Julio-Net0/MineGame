#include "server/lod_cache_selftest.h"

#include "core/fileio.h"
#include "core/log.h"
#include "platform/os_file.h"
#include "platform/os_time.h"
#include "world/chunk.h"
#include "world/lod/lod_cache.h"
#include "world/lod/lod_downsample.h"
#include "world/lod/lod_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  CASE_UNIFORM = 0,
  CASE_PACKED4_LAYERS,
  CASE_PACKED4_NOISE,
  CASE_PACKED8_LAYERS,
  CASE_PACKED8_NOISE,
  CASE_DENSE_LAYERS,
  CASE_DENSE_NOISE
} CaseId;

enum {
  CASE_COUNT = 7,
  // A column no test writes.
  MISSING_COLUMN = 100,
  FLIP_MASK = 0xFF,
  // Offset of a region header's entry count, and its width.
  ENTRY_COUNT_AT = 32,
  ENTRY_COUNT_BYTES = 4,
  CORRUPT_SHAPES = 4,
  // Distinct values of the dense cases, above what PACKED8 holds.
  DENSE_LAYER_VALUES = 16,
  DENSE_HALF_OFFSET = 1000,
  NOISE_PALETTE = 16,
  LAYER_PALETTE = 100,
  COLUMN_Y_BASE = -3,
  // Columns of the round-trip write, spread over regions and both signs.
  SPREAD_COLUMNS = 4,
  CORRUPT_OFFSET = 200,
  CHECK_TEXT_CHARS = 160,
  PRECEDENCE_LEVEL = 1,
  PRECEDENCE_CHUNKS = 4,
  BLOCK_STONE_ID = 3,
  BLOCK_DIRT_ID = 2,
  BLOCK_GRASS_ID = 4,
  BIOME_ID = 7,
  MICROS_PER_SECOND = 1000000
};

#define SCRATCH_DIR "lod_cache_selftest.tmp"
#define TEST_SEED 0x1234ABCD5678EF01ULL
#define TEST_BLOCKS 0x0F1E2D3C4B5A6978ULL
#define NOISE_MUL 6364136223846793005ULL
#define NOISE_ADD 1442695040888963407ULL
#define NOISE_SHIFT 33U
#define PRESENT_PATTERN 0xF0F0F0F0AAAA5555ULL
#define INGESTED_PATTERN 0x00000000FFFF0000ULL

static bool Check(bool Condition, const char *What) {
  if (!Condition) {
    LogError("LOD_TEST: FAIL %s", What);
  }
  return Condition;
}

static uint64_t NextNoise(uint64_t *State) {
  *State = (*State * NOISE_MUL) + NOISE_ADD;
  return *State >> NOISE_SHIFT;
}

// --- scratch directory ------------------------------------------------------

static void RemoveTree(const char *Path) {
  DIR *Dir = opendir(Path);
  if (Dir == NULL) {
    return;
  }
  char Child[LOD_CACHE_PATH_CAPACITY];
  for (struct dirent *Entry = readdir(Dir); Entry != NULL; Entry = readdir(Dir)) {
    if (strcmp(Entry->d_name, ".") == 0 || strcmp(Entry->d_name, "..") == 0) {
      continue;
    }
    snprintf(Child, sizeof(Child), "%s/%s", Path, Entry->d_name);
    if (IsDirectory(Child)) {
      RemoveTree(Child);
    } else {
      (void)remove(Child);
    }
  }
  closedir(Dir);
  (void)OsRemoveDirectory(Path);
}

static int CountFiles(const char *Path) {
  DIR *Dir = opendir(Path);
  if (Dir == NULL) {
    return 0;
  }
  int Count = 0;
  for (struct dirent *Entry = readdir(Dir); Entry != NULL; Entry = readdir(Dir)) {
    Count += (Entry->d_name[0] != '.') ? 1 : 0;
  }
  closedir(Dir);
  return Count;
}

static LodCache *OpenCache(const char *Dir, uint64_t Seed, uint64_t Blocks,
                           uint32_t Worldgen, uint32_t Format, bool Writable) {
  LodCacheConfig Config = {.Directory = Dir,
                           .Seed = Seed,
                           .BlockTableHash = Blocks,
                           .WorldgenVersion = Worldgen,
                           .FormatVersion = Format,
                           .Writable = Writable};
  return LodCacheOpen(&Config);
}

static LodCache *OpenDefault(const char *Dir, bool Writable) {
  return OpenCache(Dir, TEST_SEED, TEST_BLOCKS, (uint32_t)WORLDGEN_VERSION,
                   LOD_CACHE_FORMAT_VERSION, Writable);
}

// --- test sections ----------------------------------------------------------

typedef struct {
  const char *Name;
  LodStorageMode Mode;
  // Expected record: run-length encoded or raw (uniform has neither).
  bool ExpectRle;
} CaseInfo;

static const CaseInfo CASES[CASE_COUNT] = {
    {"uniform", LOD_STORAGE_UNIFORM, false},
    {"packed4 layers", LOD_STORAGE_PACKED4, true},
    {"packed4 noise", LOD_STORAGE_PACKED4, false},
    {"packed8 layers", LOD_STORAGE_PACKED8, true},
    {"packed8 noise", LOD_STORAGE_PACKED8, false},
    {"dense layers", LOD_STORAGE_DENSE, true},
    {"dense noise", LOD_STORAGE_DENSE, false},
};

static void FillCase(int Case, LodVoxel *Grid) {
  uint64_t Noise = (uint64_t)Case + 1U;
  for (int X = 0; X < LOD_SECTION_SIZE; X++) {
    for (int Y = 0; Y < LOD_SECTION_SIZE; Y++) {
      for (int Z = 0; Z < LOD_SECTION_SIZE; Z++) {
        unsigned Value = 0;
        switch ((CaseId)Case) {
        case CASE_UNIFORM:
          Value = BLOCK_STONE_ID;
          break;
        case CASE_PACKED4_LAYERS:
          Value = (unsigned)(Y % LOD_PACKED4_MAX_PALETTE) + 1U;
          break;
        case CASE_PACKED4_NOISE:
          Value = (unsigned)(NextNoise(&Noise) % NOISE_PALETTE) + 1U;
          break;
        case CASE_PACKED8_LAYERS:
          Value = (unsigned)(((Y * LOD_SECTION_SIZE) + X) % LAYER_PALETTE) + 1U;
          break;
        case CASE_PACKED8_NOISE:
          Value = (unsigned)(NextNoise(&Noise) % LOD_PACKED8_MAX_PALETTE) + 1U;
          break;
        case CASE_DENSE_LAYERS:
          Value = (unsigned)((X * DENSE_LAYER_VALUES) + Y) + 1U +
                  ((Z >= LOD_SECTION_SIZE / 2) ? (unsigned)DENSE_HALF_OFFSET : 0U);
          break;
        case CASE_DENSE_NOISE:
        default:
          Value = (unsigned)(NextNoise(&Noise) & LOD_VOXEL_ID_MASK);
          break;
        }
        Grid[LodVoxelIndex(X, Y, Z)] =
            LodVoxelMake((BlockID)(Value & LOD_VOXEL_ID_MASK), BIOME_ID);
      }
    }
  }
}

static bool BuildCase(int Case, LodVoxel *Grid, LodSection *Out) {
  FillCase(Case, Grid);
  LodSectionInit(Out);
  if (!LodSectionEncode(Out, Grid)) {
    return false;
  }
  Out->PresentCells = PRESENT_PATTERN ^ (uint64_t)Case;
  Out->IngestedCells = INGESTED_PATTERN << (unsigned)Case;
  return true;
}

static bool SameSection(const LodSection *A, const LodSection *B) {
  return A->Mode == B->Mode && A->PaletteCount == B->PaletteCount &&
         A->Uniform == B->Uniform && A->StorageBytes == B->StorageBytes &&
         (A->StorageBytes == 0U || memcmp(A->Storage, B->Storage, A->StorageBytes) == 0) &&
         A->PresentCells == B->PresentCells && A->IngestedCells == B->IngestedCells &&
         LodSectionContentHash(A, 0) == LodSectionContentHash(B, 0);
}

static void FreeRead(LodCacheEntry *Entries, int Count) {
  for (int Idx = 0; Idx < Count; Idx++) {
    LodSectionFree(&Entries[Idx].Section);
  }
}

// --- round trip -------------------------------------------------------------

static bool TestRoundTrip(LodVoxel *Grid, LodCacheEntry *Read) {
  char Dir[LOD_CACHE_PATH_CAPACITY];
  snprintf(Dir, sizeof(Dir), "%s/roundtrip", SCRATCH_DIR);
  LodCache *Cache = OpenDefault(Dir, true);
  bool Passed = Check(Cache != NULL, "round-trip cache opens");
  LodSection Sections[CASE_COUNT];
  for (int Case = 0; Case < CASE_COUNT; Case++) {
    LodSectionInit(&Sections[Case]);
  }
  double WriteSeconds = 0.0;
  double ReadSeconds = 0.0;
  int ReadSections = 0;
  char Text[CHECK_TEXT_CHARS];
  static const int SPREAD[SPREAD_COLUMNS][2] = {{0, 0}, {5, -3}, {-9, 12}, {-17, -1}};
  for (int Case = 0; Passed && Case < CASE_COUNT; Case++) {
    Passed = Check(BuildCase(Case, Grid, &Sections[Case]), "test section encodes");
    snprintf(Text, sizeof(Text), "%s encodes as its mode", CASES[Case].Name);
    Passed = Passed && Check(Sections[Case].Mode == CASES[Case].Mode, Text);
    // Each case alone first, so the record's encoding can be checked.
    LodCacheStats Before;
    LodCacheStats After;
    LodCacheGetStats(Cache, &Before);
    LodCacheItem Item = {.Level = 2, .X = 1, .Y = COLUMN_Y_BASE + Case, .Z = 1,
                         .Section = &Sections[Case]};
    double Start = OsGetTimeSeconds();
    Passed = Passed && Check(LodCacheWrite(Cache, &Item, 1), "single write");
    WriteSeconds += OsGetTimeSeconds() - Start;
    LodCacheGetStats(Cache, &After);
    bool Rle = After.RleSections > Before.RleSections;
    bool Raw = After.RawSections > Before.RawSections;
    const char *Encoding = CASES[Case].ExpectRle ? "run-length" : "raw";
    if (CASES[Case].Mode == LOD_STORAGE_UNIFORM) {
      Encoding = "uniform";
    }
    snprintf(Text, sizeof(Text), "%s takes the %s encoding", CASES[Case].Name,
             Encoding);
    if (CASES[Case].Mode == LOD_STORAGE_UNIFORM) {
      Passed = Passed && Check(!Rle && !Raw, Text);
    } else {
      Passed = Passed && Check(Rle == CASES[Case].ExpectRle && Raw == !CASES[Case].ExpectRle, Text);
    }
  }
  // All of them again in one call, over several regions and both signs.
  LodCacheItem Items[CASE_COUNT * SPREAD_COLUMNS];
  int ItemCount = 0;
  for (int Column = 0; Passed && Column < SPREAD_COLUMNS; Column++) {
    for (int Case = 0; Case < CASE_COUNT; Case++) {
      Items[ItemCount++] = (LodCacheItem){.Level = 3,
                                          .X = SPREAD[Column][0],
                                          .Y = COLUMN_Y_BASE + Case,
                                          .Z = SPREAD[Column][1],
                                          .Section = &Sections[Case]};
    }
  }
  Passed = Passed && Check(LodCacheWrite(Cache, Items, ItemCount), "batch write");
  for (int Column = -1; Passed && Column < SPREAD_COLUMNS; Column++) {
    int Level = (Column < 0) ? 2 : 3;
    int X = (Column < 0) ? 1 : SPREAD[Column][0];
    int Z = (Column < 0) ? 1 : SPREAD[Column][1];
    bool Generated = true;
    double Start = OsGetTimeSeconds();
    int Count = LodCacheReadColumn(Cache, Level, X, Z, Read,
                                   LOD_CACHE_MAX_COLUMN_SECTIONS, &Generated);
    ReadSeconds += OsGetTimeSeconds() - Start;
    Passed = Check(Count == CASE_COUNT, "column reads back every section") &&
             Check(!Generated, "column not cached as generated");
    for (int Case = 0; Passed && Case < CASE_COUNT; Case++) {
      snprintf(Text, sizeof(Text), "%s round trip is bit-exact", CASES[Case].Name);
      Passed = Check(Read[Case].Y == COLUMN_Y_BASE + Case, "sections lowest first") &&
               Check(SameSection(&Read[Case].Section, &Sections[Case]), Text);
    }
    ReadSections += (Count > 0) ? Count : 0;
    FreeRead(Read, (Count > 0) ? Count : 0);
  }
  bool Generated = false;
  Passed = Passed &&
           Check(LodCacheReadColumn(Cache, 3, MISSING_COLUMN, MISSING_COLUMN, Read,
                                    LOD_CACHE_MAX_COLUMN_SECTIONS, &Generated) == -1,
                 "missing region misses");

  // Replacing one record keeps the others, and the generated flag sticks.
  Sections[0].GeneratedColumn = true;
  LodSection Replacement;
  Passed = Passed && Check(BuildCase(1, Grid, &Replacement), "replacement encodes");
  Replacement.IngestedCells = ~(uint64_t)0U;
  Replacement.GeneratedColumn = true;
  LodCacheItem Replace = {.Level = 3, .X = 0, .Y = COLUMN_Y_BASE, .Z = 0,
                          .Section = &Replacement};
  Passed = Passed && Check(LodCacheWrite(Cache, &Replace, 1), "replacement write");
  int Count = Passed ? LodCacheReadColumn(Cache, 3, 0, 0, Read,
                                          LOD_CACHE_MAX_COLUMN_SECTIONS, &Generated)
                     : 0;
  Passed = Passed && Check(Count == CASE_COUNT && Generated,
                           "replacement keeps the column and flags it generated") &&
           Check(SameSection(&Read[0].Section, &Replacement), "replaced record read") &&
           Check(SameSection(&Read[1].Section, &Sections[1]), "other records kept");
  FreeRead(Read, (Count > 0) ? Count : 0);
  LodSectionFree(&Replacement);

  LodCacheStats Stats;
  if (Cache != NULL) {
    LodCacheGetStats(Cache, &Stats);
    LogInfo("LOD_TEST: ok cache round trip of %d storage cases, %d regions "
            "(%.1f us/section write, %.1f us/section read, %.2fx smaller than "
            "in memory)",
            CASE_COUNT, Stats.Regions,
            WriteSeconds * MICROS_PER_SECOND / (double)CASE_COUNT,
            (ReadSections > 0) ? ReadSeconds * MICROS_PER_SECOND / (double)ReadSections : 0.0,
            (Stats.RecordBytesWritten > 0U)
                ? (double)Stats.MemoryBytesWritten / (double)Stats.RecordBytesWritten
                : 0.0);
  }
  for (int Case = 0; Case < CASE_COUNT; Case++) {
    LodSectionFree(&Sections[Case]);
  }
  LodCacheClose(Cache);
  return Passed;
}

// --- identity ---------------------------------------------------------------

static bool WriteOne(LodCache *Cache, LodVoxel *Grid, int Level, int X, int Z) {
  LodSection Section;
  bool Passed = BuildCase(1, Grid, &Section);
  LodCacheItem Item = {.Level = Level, .X = X, .Y = 0, .Z = Z, .Section = &Section};
  Passed = Passed && LodCacheWrite(Cache, &Item, 1);
  LodSectionFree(&Section);
  return Passed;
}

static int ReadCount(LodCache *Cache, LodCacheEntry *Read, int Level, int X, int Z) {
  bool Generated = false;
  int Count = LodCacheReadColumn(Cache, Level, X, Z, Read,
                                 LOD_CACHE_MAX_COLUMN_SECTIONS, &Generated);
  FreeRead(Read, (Count > 0) ? Count : 0);
  return Count;
}

static bool TestInvalidation(LodVoxel *Grid, LodCacheEntry *Read) {
  char Dir[LOD_CACHE_PATH_CAPACITY];
  snprintf(Dir, sizeof(Dir), "%s/identity", SCRATCH_DIR);
  typedef struct {
    const char *What;
    uint64_t Seed;
    uint64_t Blocks;
    uint32_t Worldgen;
    uint32_t Format;
  } Change;
  const Change CHANGES[] = {
      {"seed", TEST_SEED + 1U, TEST_BLOCKS, (uint32_t)WORLDGEN_VERSION,
       LOD_CACHE_FORMAT_VERSION},
      {"generator version", TEST_SEED, TEST_BLOCKS, (uint32_t)WORLDGEN_VERSION + 1U,
       LOD_CACHE_FORMAT_VERSION},
      {"format version", TEST_SEED, TEST_BLOCKS, (uint32_t)WORLDGEN_VERSION,
       LOD_CACHE_FORMAT_VERSION + 1U},
      {"block registry", TEST_SEED, TEST_BLOCKS ^ 1U, (uint32_t)WORLDGEN_VERSION,
       LOD_CACHE_FORMAT_VERSION},
  };
  bool Passed = true;
  char Text[CHECK_TEXT_CHARS];
  for (size_t Idx = 0; Passed && Idx < sizeof(CHANGES) / sizeof(CHANGES[0]); Idx++) {
    const Change *Next = &CHANGES[Idx];
    LodCache *Cache = OpenDefault(Dir, true);
    Passed = Check(Cache != NULL && WriteOne(Cache, Grid, 1, 0, 0) &&
                       ReadCount(Cache, Read, 1, 0, 0) == 1,
                   "identity cache written");
    LodCacheClose(Cache);

    // A read-only open of another identity neither reads nor removes it.
    Cache = OpenCache(Dir, Next->Seed, Next->Blocks, Next->Worldgen, Next->Format,
                      false);
    int Files = CountFiles(Dir);
    snprintf(Text, sizeof(Text), "read-only cache of another %s misses and keeps files",
             Next->What);
    Passed = Passed && Check(Cache != NULL && ReadCount(Cache, Read, 1, 0, 0) == -1 &&
                                 CountFiles(Dir) == Files && Files >= 2,
                             Text);
    LodCacheClose(Cache);

    Cache = OpenCache(Dir, Next->Seed, Next->Blocks, Next->Worldgen, Next->Format,
                      true);
    LodCacheStats Stats = {0};
    if (Cache != NULL) {
      LodCacheGetStats(Cache, &Stats);
    }
    snprintf(Text, sizeof(Text), "changed %s discards the cache", Next->What);
    Passed = Passed && Check(Cache != NULL && Stats.Invalidated && Stats.Regions == 0 &&
                                 ReadCount(Cache, Read, 1, 0, 0) == -1,
                             Text);
    LodCacheClose(Cache);

    // Back to the original identity: the old data is gone, never mixed in.
    Cache = OpenDefault(Dir, true);
    snprintf(Text, sizeof(Text), "data from before the %s change is gone", Next->What);
    Passed = Passed && Check(Cache != NULL && ReadCount(Cache, Read, 1, 0, 0) == -1, Text);
    LodCacheClose(Cache);
  }
  if (Passed) {
    LogInfo("LOD_TEST: ok cache discarded when the seed, generator, block registry "
            "or format changes");
  }
  return Passed;
}

// --- corruption -------------------------------------------------------------

static bool RewriteFile(const char *Path, const unsigned char *Bytes, int Size) {
  FILE *File = fopen(Path, "wb");
  if (File == NULL) {
    return false;
  }
  bool Written = Size == 0 || fwrite(Bytes, 1, (size_t)Size, File) == (size_t)Size;
  return (fclose(File) == 0) && Written;
}

static bool TestCorruption(LodVoxel *Grid, LodCacheEntry *Read) {
  char Dir[LOD_CACHE_PATH_CAPACITY];
  char Path[LOD_CACHE_PATH_CAPACITY];
  snprintf(Dir, sizeof(Dir), "%s/corrupt", SCRATCH_DIR);
  snprintf(Path, sizeof(Path), "%s/l1.0.0.lod", Dir);
  LodCache *Cache = OpenDefault(Dir, true);
  bool Passed = Check(Cache != NULL && WriteOne(Cache, Grid, 1, 0, 0) &&
                          WriteOne(Cache, Grid, 1, 1, 0),
                      "corruption cache written");
  int Size = 0;
  unsigned char *Good = Passed ? ReadBinaryFile(Path, &Size) : NULL;
  Passed = Passed && Check(Good != NULL && Size > CORRUPT_OFFSET, "region file read");
  unsigned char *Bad = Passed ? (unsigned char *)malloc((size_t)Size) : NULL;
  Passed = Passed && Check(Bad != NULL, "corruption scratch");
  LodCacheStats Before = {0};
  LodCacheStats After = {0};

  // A flipped byte in the last record: that column misses, the other reads.
  if (Passed) {
    memcpy(Bad, Good, (size_t)Size);
    Bad[Size - 1] ^= (unsigned char)FLIP_MASK;
    LodCacheGetStats(Cache, &Before);
  }
  Passed = Passed && RewriteFile(Path, Bad, Size);
  int First = Passed ? ReadCount(Cache, Read, 1, 0, 0) : 0;
  int Second = Passed ? ReadCount(Cache, Read, 1, 1, 0) : 0;
  if (Cache != NULL) {
    LodCacheGetStats(Cache, &After);
  }
  Passed = Passed && Check((First == -1) != (Second == -1) && After.Corrupt > Before.Corrupt,
                           "a corrupted record misses alone and is counted");
  // The next write of the region drops the bad record and stays readable.
  Passed = Passed && Check(WriteOne(Cache, Grid, 1, 2, 0) &&
                               ReadCount(Cache, Read, 1, 2, 0) == 1 &&
                               ReadCount(Cache, Read, 1, (First == -1) ? 1 : 0, 0) == 1,
                           "region rewritten after a bad record");

  // Truncated, garbage, empty, and a header claiming too many entries.
  static const char *SHAPES[] = {"truncated", "garbage", "empty", "oversized index"};
  for (int Shape = 0; Passed && Shape < CORRUPT_SHAPES; Shape++) {
    memcpy(Bad, Good, (size_t)Size);
    int BadSize = Size;
    if (Shape == 0) {
      BadSize = Size / 2;
    } else if (Shape == 1) {
      uint64_t Noise = 1;
      for (int Idx = 0; Idx < Size; Idx++) {
        Bad[Idx] = (unsigned char)NextNoise(&Noise);
      }
    } else if (Shape == 2) {
      BadSize = 0;
    } else {
      memset(Bad + ENTRY_COUNT_AT, FLIP_MASK, ENTRY_COUNT_BYTES);
    }
    char Text[CHECK_TEXT_CHARS];
    snprintf(Text, sizeof(Text), "%s region misses without a crash", SHAPES[Shape]);
    Passed = Check(RewriteFile(Path, Bad, BadSize) && ReadCount(Cache, Read, 1, 0, 0) == -1 &&
                       ReadCount(Cache, Read, 1, 1, 0) == -1,
                   Text);
    snprintf(Text, sizeof(Text), "%s region is rewritten cleanly", SHAPES[Shape]);
    Passed = Passed && Check(WriteOne(Cache, Grid, 1, 0, 0) &&
                                 ReadCount(Cache, Read, 1, 0, 0) == 1,
                             Text);
  }
  free(Good);
  free(Bad);
  LodCacheClose(Cache);
  if (Passed) {
    LogInfo("LOD_TEST: ok corrupted, truncated and garbage regions miss and are "
            "rewritten");
  }
  return Passed;
}

// --- precedence across sessions ---------------------------------------------

static void LayeredPatch(LodPatch8 *Patch, BlockID Top, BlockID Below) {
  for (int X = 0; X < LOD_PATCH_SIZE; X++) {
    for (int Y = 0; Y < LOD_PATCH_SIZE; Y++) {
      for (int Z = 0; Z < LOD_PATCH_SIZE; Z++) {
        BlockID Id = Below;
        if (Y >= LOD_PATCH_SIZE / 2) {
          Id = (BlockID)BLOCK_ID_AIR;
        } else if (Y == (LOD_PATCH_SIZE / 2) - 1) {
          Id = Top;
        }
        Patch->Voxels[LodPatchIndex(X, Y, Z)] = LodCanonicalVoxel(Id, BIOME_ID);
      }
    }
  }
}

static bool SaveStore(LodCache *Cache, const LodStore *Store) {
  LodCacheItem *Items =
      (LodCacheItem *)malloc(sizeof(LodCacheItem) * ((size_t)Store->Count + 1U));
  if (Items == NULL) {
    return false;
  }
  int Count = 0;
  for (int Idx = 0; Idx < Store->Capacity; Idx++) {
    const LodStoreSlot *Slot = &Store->Slots[Idx];
    if (Slot->Level > 0) {
      Items[Count++] = (LodCacheItem){.Level = Slot->Level,
                                      .X = Slot->X,
                                      .Y = Slot->Y,
                                      .Z = Slot->Z,
                                      .Section = Slot->Section};
    }
  }
  bool Passed = LodCacheWrite(Cache, Items, Count);
  free(Items);
  return Passed;
}

static uint64_t StoreHash(const LodStore *Store) {
  uint64_t Hash = 0;
  for (int Level = 1; Level <= LOD_MAX_LEVEL; Level++) {
    Hash = (Hash * NOISE_MUL) ^ LodStoreLevelHash(Store, Level);
  }
  return Hash;
}

// Every column of Source's level-Level sections, read from the cache into
// Target the way the fill writes a cache hit.
static bool ReloadStore(LodCache *Cache, const LodStore *Source, LodStore *Target,
                        LodCacheEntry *Read) {
  bool Passed = true;
  for (int Idx = 0; Passed && Idx < Source->Capacity; Idx++) {
    const LodStoreSlot *Slot = &Source->Slots[Idx];
    if (Slot->Level <= 0 || LodStoreFind(Target, Slot->Level, Slot->X, Slot->Y, Slot->Z) != NULL) {
      continue;
    }
    bool Generated = false;
    int Count = LodCacheReadColumn(Cache, Slot->Level, Slot->X, Slot->Z, Read,
                                   LOD_CACHE_MAX_COLUMN_SECTIONS, &Generated);
    Passed = Count > 0;
    for (int Entry = 0; Passed && Entry < Count; Entry++) {
      LodSection *Section = &Read[Entry].Section;
      Passed = LodStoreWriteCached(Target, Slot->Level, Slot->X, Read[Entry].Y, Slot->Z,
                                   Section, Section->PresentCells,
                                   Section->IngestedCells, Generated) == LOD_WRITE_APPLIED;
    }
    FreeRead(Read, (Count > 0) ? Count : 0);
  }
  return Passed;
}

static bool TestPrecedenceReload(LodVoxel *Grid, LodCacheEntry *Read) {
  char Dir[LOD_CACHE_PATH_CAPACITY];
  snprintf(Dir, sizeof(Dir), "%s/precedence", SCRATCH_DIR);
  LodStoreConfig Config;
  LodStoreDefaultConfig(&Config);
  LodStore Original;
  LodStore Reloaded;
  bool OriginalReady = LodStoreInit(&Original, &Config);
  bool ReloadedReady = LodStoreInit(&Reloaded, &Config);
  LodCache *Cache = OpenDefault(Dir, true);
  bool Passed = Check(OriginalReady && ReloadedReady && Cache != NULL,
                      "precedence stores and cache");

  // Synthetic level-1 sections everywhere, then real chunks into part of them
  // (propagated up every level), like a session that built something.
  LodSection Synthetic;
  LodSectionInit(&Synthetic);
  for (int X = 0; Passed && X < 2; X++) {
    for (int Z = 0; Z < 2; Z++) {
      Passed = BuildCase(1, Grid, &Synthetic) &&
               LodStoreWriteSynthetic(&Original, PRECEDENCE_LEVEL, X, 0, Z, &Synthetic,
                                      ~(uint64_t)0U) == LOD_WRITE_APPLIED;
      LodSectionFree(&Synthetic);
    }
  }
  LodPatch8 Real;
  LayeredPatch(&Real, (BlockID)BLOCK_GRASS_ID, (BlockID)BLOCK_DIRT_ID);
  for (int Chunk = 0; Passed && Chunk < PRECEDENCE_CHUNKS; Chunk++) {
    Passed = LodStoreWritePatch(&Original, 1, Chunk, 0, Chunk / 2, &Real,
                                LOD_ORIGIN_INGESTED) == LOD_WRITE_APPLIED;
  }
  Passed = Check(Passed && LodStoreFlush(&Original), "original store built");
  const LodSection *Built = LodStoreFind(&Original, 1, 0, 0, 0);
  Passed = Passed && Check(Built != NULL && Built->IngestedCells != 0U &&
                               Built->IngestedCells != ~(uint64_t)0U,
                           "original mixes real and synthetic cells");
  Passed = Passed && Check(SaveStore(Cache, &Original), "original saved");
  LodCacheClose(Cache);

  // A new session: a fresh store loaded from a reopened cache.
  Cache = OpenDefault(Dir, true);
  Passed = Passed && Check(Cache != NULL && ReloadStore(Cache, &Original, &Reloaded, Read),
                           "reloaded from the cache") &&
           Check(StoreHash(&Reloaded) == StoreHash(&Original) && Reloaded.Count == Original.Count,
                 "reloaded store equals the saved one");

  // Synthetic data written over the reload leaves every real cell alone.
  const LodSection *Mixed = LodStoreFind(&Reloaded, 1, 0, 0, 0);
  uint64_t Ingested = (Mixed != NULL) ? Mixed->IngestedCells : 0U;
  LodVoxel *Before = (LodVoxel *)malloc(sizeof(LodVoxel) * LOD_SECTION_VOLUME);
  Passed = Passed && Check(Before != NULL && Mixed != NULL && Ingested == Built->IngestedCells,
                           "real cells reloaded as real");
  if (Passed) {
    LodSectionDecode(Mixed, Before);
    Passed = Check(BuildCase(4, Grid, &Synthetic) &&
                       LodStoreWriteSynthetic(&Reloaded, 1, 0, 0, 0, &Synthetic,
                                              ~(uint64_t)0U) == LOD_WRITE_APPLIED,
                   "synthetic write over the reload");
    LodSectionFree(&Synthetic);
  }
  bool RealKept = Passed;
  for (int X = 0; RealKept && X < LOD_SECTION_SIZE; X++) {
    for (int Y = 0; Y < LOD_SECTION_SIZE; Y++) {
      for (int Z = 0; Z < LOD_SECTION_SIZE; Z++) {
        uint64_t Bit = LodCellBit(X / LOD_CELL_SIZE, Y / LOD_CELL_SIZE, Z / LOD_CELL_SIZE);
        if ((Ingested & Bit) != 0U &&
            LodSectionGet(Mixed, X, Y, Z) != Before[LodVoxelIndex(X, Y, Z)]) {
          RealKept = false;
        }
      }
    }
  }
  Passed = Passed && Check(RealKept && Mixed->IngestedCells == Ingested,
                           "real cells from the cache keep precedence over synthetic data");

  // Cached real cells never replace this session's real cells.
  LodPatch8 Newer;
  LodStore Live;
  bool LiveReady = LodStoreInit(&Live, &Config);
  LayeredPatch(&Newer, (BlockID)BLOCK_STONE_ID, (BlockID)BLOCK_STONE_ID);
  Passed = Passed && Check(LiveReady && LodStoreWritePatch(&Live, 1, 0, 0, 0, &Newer,
                                                           LOD_ORIGIN_INGESTED) ==
                                            LOD_WRITE_APPLIED,
                           "live real chunk written");
  const LodSection *LiveSection = LiveReady ? LodStoreFind(&Live, 1, 0, 0, 0) : NULL;
  LodVoxel LiveVoxel = (LiveSection != NULL) ? LodSectionGet(LiveSection, 0, 0, 0) : 0U;
  bool Generated = false;
  int Count = Passed ? LodCacheReadColumn(Cache, 1, 0, 0, Read,
                                          LOD_CACHE_MAX_COLUMN_SECTIONS, &Generated)
                     : 0;
  Passed = Passed && Check(Count > 0, "cached column read for the live store");
  for (int Entry = 0; Passed && Entry < Count; Entry++) {
    LodSection *Section = &Read[Entry].Section;
    (void)LodStoreWriteCached(&Live, 1, 0, Read[Entry].Y, 0, Section,
                              Section->PresentCells, Section->IngestedCells, false);
  }
  FreeRead(Read, (Count > 0) ? Count : 0);
  LiveSection = LiveReady ? LodStoreFind(&Live, 1, 0, 0, 0) : NULL;
  Passed = Passed && Check(LiveSection != NULL && LodSectionGet(LiveSection, 0, 0, 0) == LiveVoxel,
                           "this session's real cells win over cached real cells");
  if (LiveReady) {
    LodStoreFree(&Live);
  }
  free(Before);
  LodCacheClose(Cache);
  if (OriginalReady) {
    LodStoreFree(&Original);
  }
  if (ReloadedReady) {
    LodStoreFree(&Reloaded);
  }
  if (Passed) {
    LogInfo("LOD_TEST: ok real cells survive a save and reload with their "
            "precedence");
  }
  return Passed;
}

// --- read-only --------------------------------------------------------------

static bool TestReadOnly(LodVoxel *Grid, LodCacheEntry *Read) {
  char Dir[LOD_CACHE_PATH_CAPACITY];
  snprintf(Dir, sizeof(Dir), "%s/readonly/nested", SCRATCH_DIR);
  LodCache *Cache = OpenDefault(Dir, false);
  bool Passed = Check(Cache != NULL, "read-only cache opens without a directory") &&
                Check(!IsDirectory(Dir), "read-only open creates no directory") &&
                Check(!WriteOne(Cache, Grid, 1, 0, 0), "read-only write refused") &&
                Check(ReadCount(Cache, Read, 1, 0, 0) == -1, "read-only read misses") &&
                Check(!IsDirectory(Dir) && CountFiles(SCRATCH_DIR "/readonly") == 0,
                      "read-only cache created no file");
  LodCacheClose(Cache);

  // Over an existing cache: reads work, nothing changes on disk.
  char Existing[LOD_CACHE_PATH_CAPACITY];
  snprintf(Existing, sizeof(Existing), "%s/roundtrip", SCRATCH_DIR);
  int Files = CountFiles(Existing);
  Cache = OpenDefault(Existing, false);
  Passed = Passed && Check(Cache != NULL && ReadCount(Cache, Read, 3, 0, 0) == CASE_COUNT &&
                               !WriteOne(Cache, Grid, 3, 0, 0) &&
                               CountFiles(Existing) == Files,
                           "read-only cache reads an existing cache and writes nothing");
  LodCacheClose(Cache);
  if (Passed) {
    LogInfo("LOD_TEST: ok a read-only cache creates and writes nothing");
  }
  return Passed;
}

bool RunLodCacheSelfTest(void) {
  RemoveTree(SCRATCH_DIR);
  LodVoxel *Grid = (LodVoxel *)malloc(sizeof(LodVoxel) * LOD_SECTION_VOLUME);
  LodCacheEntry *Read = (LodCacheEntry *)calloc(LOD_CACHE_MAX_COLUMN_SECTIONS,
                                                sizeof(LodCacheEntry));
  bool Passed = Check(Grid != NULL && Read != NULL && OsMakeDirectories(SCRATCH_DIR),
                      "cache test scratch");
  Passed = Passed && TestRoundTrip(Grid, Read) && TestInvalidation(Grid, Read) &&
           TestCorruption(Grid, Read) && TestPrecedenceReload(Grid, Read) &&
           TestReadOnly(Grid, Read);
  free(Grid);
  free(Read);
  RemoveTree(SCRATCH_DIR);
  Passed = Passed && Check(!IsDirectory(SCRATCH_DIR), "cache test scratch removed");
  return Passed;
}
