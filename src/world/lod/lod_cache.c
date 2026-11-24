#include "world/lod/lod_cache.h"

#include "core/fileio.h"
#include "core/log.h"
#include "platform/os_file.h"
#include "platform/os_time.h"
#include "world/lod/lod_store.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  REGION_COLUMN_COUNT = LOD_CACHE_REGION_COLUMNS * LOD_CACHE_REGION_COLUMNS,
  MAX_REGION_ENTRIES = REGION_COLUMN_COUNT * LOD_CACHE_MAX_COLUMN_SECTIONS,
  MAGIC_BYTES = 4,
  // Region header: magic, format, generator, level, seed, region x and z,
  // entry count, reserved, generated-column mask, checksum.
  HEADER_BYTES = 56,
  HEADER_CHECKSUM_AT = 48,
  INDEX_ENTRY_BYTES = 24,
  // Record header: present and ingested cells, mode, encoding, palette count,
  // uniform voxel, payload bytes.
  RECORD_HEADER_BYTES = 28,
  META_BYTES = 32,
  META_CHECKSUM_AT = 24,
  PACKED4_INDEX_BYTES = LOD_SECTION_VOLUME / 2,
  PACKED8_INDEX_BYTES = LOD_SECTION_VOLUME,
  MAX_PALETTE_BYTES = LOD_PACKED8_MAX_PALETTE * (int)sizeof(LodVoxel),
  MAX_RECORD_BYTES = RECORD_HEADER_BYTES + MAX_PALETTE_BYTES + LOD_DENSE_BYTES,
  LOCK_STRIPES = 32,
  // PackBits-style runs: a control byte below RLE_REPEAT_FLAG starts a literal
  // of (control + 1) elements, one at or above it a repeat of
  // (control - RLE_REPEAT_FLAG + minimum run) copies of the next element.
  RLE_REPEAT_FLAG = 0x80,
  RLE_CONTROL_MAX = 0x7F,
  RLE_MAX_LITERAL = RLE_CONTROL_MAX + 1,
  ENCODING_RAW = 0,
  ENCODING_RLE = 1,
  CORRUPT_WARNINGS = 8,
  BYTE_BITS = 8,
  BYTE_MASK = 0xFF,
  WORD_BYTES = 4,
  DWORD_BYTES = 8,
  INITIAL_OUTPUT_BYTES = 65536,
  // Field offsets of a record.
  RECORD_INGESTED_AT = 8,
  RECORD_MODE_AT = 16,
  RECORD_ENCODING_AT = 17,
  RECORD_PALETTE_COUNT_AT = 18,
  RECORD_UNIFORM_AT = 20,
  RECORD_PAYLOAD_BYTES_AT = 24,
  // Field offsets of a region header (identity from IDENTITY_AT on).
  IDENTITY_AT = 4,
  HEADER_LEVEL_AT = 12,
  HEADER_SEED_AT = 16,
  HEADER_REGION_X_AT = 24,
  HEADER_REGION_Z_AT = 28,
  HEADER_ENTRIES_AT = 32,
  HEADER_RESERVED_AT = 36,
  HEADER_MASK_AT = 40,
  // Field offsets of an index entry.
  ENTRY_RESERVED_AT = 2,
  ENTRY_Y_AT = 4,
  ENTRY_OFFSET_AT = 8,
  ENTRY_SIZE_AT = 12,
  ENTRY_CHECKSUM_AT = 16,
  META_SEED_AT = 16,
  // Room left in a path for a region file name.
  PATH_NAME_ROOM = 32,
  KNOWN_INITIAL = 256,
  REPLACE_ATTEMPTS = 8,
  REPLACE_WAIT_MS = 2,
  KNOWN_LEVEL_SHIFT = 56,
  KNOWN_X_SHIFT = 28,
  DECIMAL = 10,
  // Level, region x and z, column, height.
  ITEM_KEYS = 5,
  MICROS_PER_SECOND = 1000000
};

#define BYTES_PER_MEGABYTE 1048576.0
#define KNOWN_COORD_MASK 0xFFFFFFFULL
#define KNOWN_HASH_MUL 0x9E3779B97F4A7C15ULL
#define KNOWN_HASH_SHIFT 32U

#define FNV_OFFSET 0xCBF29CE484222325ULL
#define FNV_PRIME 0x100000001B3ULL
#define STRIPE_MUL_X 0x9E3779B97F4A7C15ULL
#define STRIPE_MUL_Z 0xC2B2AE3D27D4EB4FULL
#define STRIPE_SHIFT 29U

static const char REGION_MAGIC[MAGIC_BYTES] = {'M', 'G', 'L', 'R'};
static const char META_MAGIC[MAGIC_BYTES] = {'M', 'G', 'L', 'M'};
static const char META_NAME[] = "meta.bin";
static const char REGION_SUFFIX[] = ".lod";
static const char TEMP_SUFFIX[] = ".lod.tmp";

struct LodCache {
  char Directory[LOD_CACHE_PATH_CAPACITY];
  uint64_t Seed;
  uint32_t WorldgenVersion;
  uint32_t FormatVersion;
  bool Writable;
  // False when a read-only cache's identity does not match: never mix it in.
  bool Readable;
  pthread_mutex_t Stripes[LOCK_STRIPES];
  pthread_mutex_t StatsLock;
  LodCacheStats Stats;
  // Region files known to exist (open addressing, 0 is empty), so a column of
  // a region never written misses without touching the disk. Under KnownLock.
  pthread_mutex_t KnownLock;
  uint64_t *Known;
  int KnownCapacity;
  int KnownCount;
  bool KnownUnreliable;
};

// --- little-endian fields ---------------------------------------------------

static void Put16(uint8_t *Out, uint32_t Value) {
  Out[0] = (uint8_t)(Value & BYTE_MASK);
  Out[1] = (uint8_t)((Value >> BYTE_BITS) & BYTE_MASK);
}

static void Put32(uint8_t *Out, uint32_t Value) {
  for (int Idx = 0; Idx < WORD_BYTES; Idx++) {
    Out[Idx] = (uint8_t)((Value >> ((unsigned)Idx * BYTE_BITS)) & BYTE_MASK);
  }
}

static void Put64(uint8_t *Out, uint64_t Value) {
  for (int Idx = 0; Idx < DWORD_BYTES; Idx++) {
    Out[Idx] = (uint8_t)((Value >> ((unsigned)Idx * BYTE_BITS)) & BYTE_MASK);
  }
}

static uint32_t Get16(const uint8_t *In) {
  return (uint32_t)In[0] | ((uint32_t)In[1] << BYTE_BITS);
}

static uint32_t Get32(const uint8_t *In) {
  uint32_t Value = 0;
  for (int Idx = WORD_BYTES - 1; Idx >= 0; Idx--) {
    Value = (Value << BYTE_BITS) | In[Idx];
  }
  return Value;
}

static uint64_t Get64(const uint8_t *In) {
  uint64_t Value = 0;
  for (int Idx = DWORD_BYTES - 1; Idx >= 0; Idx--) {
    Value = (Value << BYTE_BITS) | In[Idx];
  }
  return Value;
}

static uint64_t Fnv(uint64_t Hash, const uint8_t *Data, size_t Size) {
  for (size_t Idx = 0; Idx < Size; Idx++) {
    Hash = (Hash ^ Data[Idx]) * FNV_PRIME;
  }
  return Hash;
}

// --- run-length coding ------------------------------------------------------

static int MinimumRun(int Width) { return (Width == 1) ? 3 : 2; }

static int RunLength(const uint8_t *Elements, int At, int Count, int Width,
                     int Limit) {
  const uint8_t *First = Elements + ((size_t)At * (size_t)Width);
  int Run = 1;
  while (At + Run < Count && Run < Limit &&
         memcmp(First, Elements + ((size_t)(At + Run) * (size_t)Width),
                (size_t)Width) == 0) {
    Run++;
  }
  return Run;
}

// Encodes Count elements of Width bytes into Out; 0 when the result would not
// fit in Capacity bytes.
static size_t RleEncode(const uint8_t *Elements, int Count, int Width,
                        uint8_t *Out, size_t Capacity) {
  int MinRun = MinimumRun(Width);
  int MaxRun = RLE_CONTROL_MAX + MinRun;
  size_t Used = 0;
  int At = 0;
  while (At < Count) {
    int Run = RunLength(Elements, At, Count, Width, MaxRun);
    if (Run >= MinRun) {
      if (Used + 1U + (size_t)Width > Capacity) {
        return 0;
      }
      Out[Used++] = (uint8_t)(RLE_REPEAT_FLAG + (Run - MinRun));
      memcpy(Out + Used, Elements + ((size_t)At * (size_t)Width), (size_t)Width);
      Used += (size_t)Width;
      At += Run;
      continue;
    }
    int Start = At;
    int Literal = 0;
    while (At < Count && Literal < RLE_MAX_LITERAL &&
           (Literal == 0 || RunLength(Elements, At, Count, Width, MinRun) < MinRun)) {
      At++;
      Literal++;
    }
    size_t Bytes = (size_t)Literal * (size_t)Width;
    if (Used + 1U + Bytes > Capacity) {
      return 0;
    }
    Out[Used++] = (uint8_t)(Literal - 1);
    memcpy(Out + Used, Elements + ((size_t)Start * (size_t)Width), Bytes);
    Used += Bytes;
  }
  return Used;
}

static bool RleDecode(const uint8_t *In, size_t Size, int Count, int Width,
                      uint8_t *Out) {
  int MinRun = MinimumRun(Width);
  size_t Read = 0;
  int Done = 0;
  while (Done < Count) {
    if (Read >= Size) {
      return false;
    }
    unsigned Control = In[Read++];
    if (Control >= (unsigned)RLE_REPEAT_FLAG) {
      int Run = (int)(Control - (unsigned)RLE_REPEAT_FLAG) + MinRun;
      if (Done + Run > Count || Read + (size_t)Width > Size) {
        return false;
      }
      for (int Copy = 0; Copy < Run; Copy++) {
        memcpy(Out + ((size_t)(Done + Copy) * (size_t)Width), In + Read,
               (size_t)Width);
      }
      Read += (size_t)Width;
      Done += Run;
    } else {
      int Literal = (int)Control + 1;
      size_t Bytes = (size_t)Literal * (size_t)Width;
      if (Done + Literal > Count || Read + Bytes > Size) {
        return false;
      }
      memcpy(Out + ((size_t)Done * (size_t)Width), In + Read, Bytes);
      Read += Bytes;
      Done += Literal;
    }
  }
  return Read == Size;
}

// Voxel index of the Order-th voxel in height-major order (y, then x, then z),
// where whole layers of terrain are equal.
static int HeightMajorIndex(int Order) {
  int Layer = LOD_SECTION_SIZE * LOD_SECTION_SIZE;
  int Y = Order / Layer;
  int X = (Order / LOD_SECTION_SIZE) % LOD_SECTION_SIZE;
  int Z = Order % LOD_SECTION_SIZE;
  return LodVoxelIndex(X, Y, Z);
}

// --- section records --------------------------------------------------------

static unsigned PackedIndex(const LodSection *Section, int Voxel) {
  const uint8_t *Indices = (const uint8_t *)Section->Storage +
                           ((size_t)Section->PaletteCount * sizeof(LodVoxel));
  if (Section->Mode == LOD_STORAGE_PACKED8) {
    return Indices[Voxel];
  }
  unsigned Shift = ((unsigned)Voxel & 1U) * (unsigned)(BYTE_BITS / 2);
  return ((unsigned)Indices[Voxel >> 1] >> Shift) & (unsigned)(BYTE_MASK >> 4);
}

typedef enum { RECORD_UNIFORM, RECORD_RLE, RECORD_RAW } RecordKind;

// Writes Section's record to Out (MAX_RECORD_BYTES); Work holds at least
// LOD_DENSE_BYTES. Returns the record size.
static size_t SerializeSection(const LodSection *Section, uint8_t *Out,
                               uint8_t *Work, RecordKind *Kind) {
  Put64(Out, Section->PresentCells);
  Put64(Out + RECORD_INGESTED_AT, Section->IngestedCells);
  Out[RECORD_MODE_AT] = (uint8_t)Section->Mode;
  Put16(Out + RECORD_PALETTE_COUNT_AT, (uint32_t)Section->PaletteCount);
  Put32(Out + RECORD_UNIFORM_AT, Section->Uniform);
  uint8_t *Payload = Out + RECORD_HEADER_BYTES;
  size_t PaletteBytes = (size_t)Section->PaletteCount * sizeof(LodVoxel);
  size_t Size = 0;
  uint8_t Encoding = ENCODING_RAW;
  *Kind = RECORD_RAW;
  if (Section->Mode == LOD_STORAGE_UNIFORM) {
    *Kind = RECORD_UNIFORM;
  } else if (Section->Mode == LOD_STORAGE_DENSE) {
    const LodVoxel *Voxels = (const LodVoxel *)Section->Storage;
    for (int Order = 0; Order < LOD_SECTION_VOLUME; Order++) {
      Put32(Work + ((size_t)Order * WORD_BYTES), Voxels[HeightMajorIndex(Order)]);
    }
    Size = RleEncode(Work, LOD_SECTION_VOLUME, WORD_BYTES, Payload,
                     (size_t)LOD_DENSE_BYTES - 1U);
    if (Size > 0U) {
      Encoding = ENCODING_RLE;
    } else {
      for (int Voxel = 0; Voxel < LOD_SECTION_VOLUME; Voxel++) {
        Put32(Payload + ((size_t)Voxel * WORD_BYTES), Voxels[Voxel]);
      }
      Size = LOD_DENSE_BYTES;
    }
  } else {
    const LodVoxel *Palette = (const LodVoxel *)Section->Storage;
    for (int Entry = 0; Entry < Section->PaletteCount; Entry++) {
      Put32(Payload + ((size_t)Entry * WORD_BYTES), Palette[Entry]);
    }
    Payload += PaletteBytes;
    size_t RawBytes = (Section->Mode == LOD_STORAGE_PACKED4)
                          ? (size_t)PACKED4_INDEX_BYTES
                          : (size_t)PACKED8_INDEX_BYTES;
    for (int Order = 0; Order < LOD_SECTION_VOLUME; Order++) {
      Work[Order] = (uint8_t)PackedIndex(Section, HeightMajorIndex(Order));
    }
    Size = RleEncode(Work, LOD_SECTION_VOLUME, 1, Payload, RawBytes - 1U);
    if (Size > 0U) {
      Encoding = ENCODING_RLE;
    } else {
      memcpy(Payload, (const uint8_t *)Section->Storage + PaletteBytes, RawBytes);
      Size = RawBytes;
    }
  }
  if (Encoding == ENCODING_RLE) {
    *Kind = RECORD_RLE;
  }
  Out[RECORD_ENCODING_AT] = Encoding;
  Put32(Out + RECORD_PAYLOAD_BYTES_AT, (uint32_t)Size);
  return (size_t)RECORD_HEADER_BYTES + PaletteBytes + Size;
}

static bool IndicesInPalette(const uint8_t *Indices, int Count, int Palette) {
  for (int Idx = 0; Idx < Count; Idx++) {
    if ((int)Indices[Idx] >= Palette) {
      return false;
    }
  }
  return true;
}

// Rebuilds a packed section's storage from Work, the indices in height-major
// order (one per byte).
static void PackIndices(const uint8_t *Work, bool FourBit, uint8_t *Indices) {
  if (FourBit) {
    memset(Indices, 0, PACKED4_INDEX_BYTES);
  }
  for (int Order = 0; Order < LOD_SECTION_VOLUME; Order++) {
    int Voxel = HeightMajorIndex(Order);
    if (FourBit) {
      unsigned Shift = ((unsigned)Voxel & 1U) * (unsigned)(BYTE_BITS / 2);
      Indices[Voxel >> 1] |= (uint8_t)((unsigned)Work[Order] << Shift);
    } else {
      Indices[Voxel] = Work[Order];
    }
  }
}

static bool DecodePacked(const uint8_t *Payload, size_t PayloadBytes,
                         uint8_t Encoding, bool FourBit, int PaletteCount,
                         uint8_t *Work, uint8_t *Indices) {
  size_t RawBytes = FourBit ? (size_t)PACKED4_INDEX_BYTES
                            : (size_t)PACKED8_INDEX_BYTES;
  if (Encoding == ENCODING_RLE) {
    if (!RleDecode(Payload, PayloadBytes, LOD_SECTION_VOLUME, 1, Work) ||
        !IndicesInPalette(Work, LOD_SECTION_VOLUME, PaletteCount)) {
      return false;
    }
    PackIndices(Work, FourBit, Indices);
    return true;
  }
  if (PayloadBytes != RawBytes) {
    return false;
  }
  memcpy(Indices, Payload, RawBytes);
  if (!FourBit) {
    return IndicesInPalette(Indices, LOD_SECTION_VOLUME, PaletteCount);
  }
  for (size_t Byte = 0; Byte < RawBytes; Byte++) {
    unsigned Pair = Indices[Byte];
    unsigned Low = Pair & (unsigned)(BYTE_MASK >> 4);
    if ((int)Low >= PaletteCount || (int)(Pair >> 4) >= PaletteCount) {
      return false;
    }
  }
  return true;
}

static bool DecodeDense(const uint8_t *Payload, size_t PayloadBytes,
                        uint8_t Encoding, uint8_t *Work, LodVoxel *Voxels) {
  if (Encoding == ENCODING_RLE) {
    if (!RleDecode(Payload, PayloadBytes, LOD_SECTION_VOLUME, WORD_BYTES, Work)) {
      return false;
    }
    for (int Order = 0; Order < LOD_SECTION_VOLUME; Order++) {
      Voxels[HeightMajorIndex(Order)] = Get32(Work + ((size_t)Order * WORD_BYTES));
    }
    return true;
  }
  if (PayloadBytes != (size_t)LOD_DENSE_BYTES) {
    return false;
  }
  for (int Voxel = 0; Voxel < LOD_SECTION_VOLUME; Voxel++) {
    Voxels[Voxel] = Get32(Payload + ((size_t)Voxel * WORD_BYTES));
  }
  return true;
}

// Parses one record into Out (initialised, storage of its own). False on any
// inconsistency; Out is then empty.
static bool DeserializeSection(const uint8_t *Record, size_t Size, uint8_t *Work,
                               LodSection *Out) {
  LodSectionInit(Out);
  if (Size < (size_t)RECORD_HEADER_BYTES) {
    return false;
  }
  unsigned Mode = Record[RECORD_MODE_AT];
  uint8_t Encoding = Record[RECORD_ENCODING_AT];
  int PaletteCount = (int)Get16(Record + RECORD_PALETTE_COUNT_AT);
  LodVoxel Uniform = Get32(Record + RECORD_UNIFORM_AT);
  size_t PayloadBytes = Get32(Record + RECORD_PAYLOAD_BYTES_AT);
  size_t PaletteBytes = (size_t)PaletteCount * sizeof(LodVoxel);
  if (Mode >= (unsigned)LOD_STORAGE_COUNT || Encoding > ENCODING_RLE ||
      (size_t)RECORD_HEADER_BYTES + PaletteBytes + PayloadBytes != Size) {
    return false;
  }
  const uint8_t *Payload = Record + RECORD_HEADER_BYTES + PaletteBytes;
  bool Valid = false;
  if (Mode == (unsigned)LOD_STORAGE_UNIFORM) {
    Valid = PaletteCount == 0 && PayloadBytes == 0U;
    Out->Uniform = Uniform;
  } else if (Mode == (unsigned)LOD_STORAGE_DENSE) {
    LodVoxel *Voxels = (PaletteCount == 0)
                           ? (LodVoxel *)malloc((size_t)LOD_DENSE_BYTES)
                           : NULL;
    Valid = Voxels != NULL && DecodeDense(Payload, PayloadBytes, Encoding, Work, Voxels);
    if (Valid) {
      Out->Storage = Voxels;
      Out->StorageBytes = LOD_DENSE_BYTES;
    } else {
      free(Voxels);
    }
  } else {
    bool FourBit = Mode == (unsigned)LOD_STORAGE_PACKED4;
    int MaxPalette = FourBit ? LOD_PACKED4_MAX_PALETTE : LOD_PACKED8_MAX_PALETTE;
    size_t IndexBytes = FourBit ? (size_t)PACKED4_INDEX_BYTES
                                : (size_t)PACKED8_INDEX_BYTES;
    uint8_t *Block = (PaletteCount >= 1 && PaletteCount <= MaxPalette)
                         ? (uint8_t *)malloc(PaletteBytes + IndexBytes)
                         : NULL;
    Valid = Block != NULL &&
            DecodePacked(Payload, PayloadBytes, Encoding, FourBit, PaletteCount,
                         Work, Block + PaletteBytes);
    if (Valid) {
      LodVoxel *Palette = (LodVoxel *)Block;
      for (int Entry = 0; Entry < PaletteCount; Entry++) {
        Palette[Entry] = Get32(Record + RECORD_HEADER_BYTES +
                               ((size_t)Entry * WORD_BYTES));
      }
      Out->Storage = Block;
      Out->StorageBytes = PaletteBytes + IndexBytes;
      Out->PaletteCount = PaletteCount;
    } else {
      free(Block);
    }
  }
  if (!Valid) {
    LodSectionInit(Out);
    return false;
  }
  Out->Mode = (LodStorageMode)Mode;
  Out->PresentCells = Get64(Record);
  Out->IngestedCells = Get64(Record + RECORD_INGESTED_AT);
  return true;
}

// --- paths, identity --------------------------------------------------------

static void RegionPath(const LodCache *Cache, int Level, int RegionX,
                       int RegionZ, const char *Suffix, char *Out) {
  snprintf(Out, LOD_CACHE_PATH_CAPACITY, "%s/l%d.%d.%d%s", Cache->Directory,
           Level, RegionX, RegionZ, Suffix);
}

static pthread_mutex_t *StripeOf(LodCache *Cache, int Level, int RegionX,
                                 int RegionZ) {
  uint64_t Hash = ((uint64_t)(uint32_t)RegionX * STRIPE_MUL_X) ^
                  ((uint64_t)(uint32_t)RegionZ * STRIPE_MUL_Z) ^ (uint64_t)Level;
  Hash ^= Hash >> STRIPE_SHIFT;
  return &Cache->Stripes[Hash % (uint64_t)LOCK_STRIPES];
}

// Never zero: levels start at 1.
static uint64_t KnownKey(int Level, int RegionX, int RegionZ) {
  return ((uint64_t)(uint32_t)Level << KNOWN_LEVEL_SHIFT) |
         (((uint64_t)(uint32_t)RegionX & KNOWN_COORD_MASK) << KNOWN_X_SHIFT) |
         ((uint64_t)(uint32_t)RegionZ & KNOWN_COORD_MASK);
}

static unsigned KnownSlot(uint64_t Key, int Capacity) {
  return (unsigned)((Key * KNOWN_HASH_MUL) >> KNOWN_HASH_SHIFT) &
         ((unsigned)Capacity - 1U);
}

static bool KnownHasLocked(const LodCache *Cache, uint64_t Key) {
  if (Cache->KnownCapacity == 0) {
    return false;
  }
  unsigned Mask = (unsigned)Cache->KnownCapacity - 1U;
  for (unsigned Idx = KnownSlot(Key, Cache->KnownCapacity);;
       Idx = (Idx + 1U) & Mask) {
    if (Cache->Known[Idx] == 0U) {
      return false;
    }
    if (Cache->Known[Idx] == Key) {
      return true;
    }
  }
}

static void KnownPlace(uint64_t *Table, int Capacity, uint64_t Key) {
  unsigned Mask = (unsigned)Capacity - 1U;
  unsigned Idx = KnownSlot(Key, Capacity);
  while (Table[Idx] != 0U) {
    Idx = (Idx + 1U) & Mask;
  }
  Table[Idx] = Key;
}

// Remembers a region file. When the table cannot grow, every lookup answers
// "present" from then on, so no region is ever skipped; reads just go to disk.
static void KnownAdd(LodCache *Cache, int Level, int RegionX, int RegionZ) {
  uint64_t Key = KnownKey(Level, RegionX, RegionZ);
  pthread_mutex_lock(&Cache->KnownLock);
  if (!KnownHasLocked(Cache, Key)) {
    if ((Cache->KnownCount + 1) * 2 > Cache->KnownCapacity) {
      int Capacity =
          (Cache->KnownCapacity > 0) ? Cache->KnownCapacity * 2 : KNOWN_INITIAL;
      uint64_t *Grown = (uint64_t *)calloc((size_t)Capacity, sizeof(uint64_t));
      if (Grown != NULL) {
        for (int Idx = 0; Idx < Cache->KnownCapacity; Idx++) {
          if (Cache->Known[Idx] != 0U) {
            KnownPlace(Grown, Capacity, Cache->Known[Idx]);
          }
        }
        free(Cache->Known);
        Cache->Known = Grown;
        Cache->KnownCapacity = Capacity;
      }
    }
    if ((Cache->KnownCount + 1) * 2 <= Cache->KnownCapacity) {
      KnownPlace(Cache->Known, Cache->KnownCapacity, Key);
      Cache->KnownCount++;
    } else {
      Cache->KnownUnreliable = true;
    }
  }
  pthread_mutex_unlock(&Cache->KnownLock);
}

static bool KnownHas(LodCache *Cache, int Level, int RegionX, int RegionZ) {
  pthread_mutex_lock(&Cache->KnownLock);
  bool Found = Cache->KnownUnreliable ||
               KnownHasLocked(Cache, KnownKey(Level, RegionX, RegionZ));
  pthread_mutex_unlock(&Cache->KnownLock);
  return Found;
}

// One "<int>" of a region file name ending at Stop; advances *Cursor past it.
static bool ParseNameInt(const char **Cursor, char Stop, int *Out) {
  char *End = NULL;
  errno = 0;
  long Value = strtol(*Cursor, &End, DECIMAL);
  if (End == *Cursor || *End != Stop || errno != 0 || Value < INT_MIN ||
      Value > INT_MAX) {
    return false;
  }
  *Out = (int)Value;
  *Cursor = End + 1;
  return true;
}

// "l<level>.<rx>.<rz>.lod".
static bool ParseRegionName(const char *Name, int *Level, int *RegionX,
                            int *RegionZ) {
  if (Name[0] != 'l') {
    return false;
  }
  const char *Cursor = Name + 1;
  return ParseNameInt(&Cursor, '.', Level) && ParseNameInt(&Cursor, '.', RegionX) &&
         ParseNameInt(&Cursor, '.', RegionZ) && strcmp(Cursor - 1, REGION_SUFFIX) == 0;
}

static bool EndsWith(const char *Name, const char *Suffix) {
  size_t Length = strlen(Name);
  size_t SuffixLength = strlen(Suffix);
  return Length > SuffixLength &&
         strcmp(Name + Length - SuffixLength, Suffix) == 0;
}

static bool IsCacheFile(const char *Name) {
  return Name[0] == 'l' && (EndsWith(Name, REGION_SUFFIX) || EndsWith(Name, TEMP_SUFFIX));
}

static long FileSize(FILE *File) {
  if (fseek(File, 0, SEEK_END) != 0) {
    return -1;
  }
  long Size = ftell(File);
  return (fseek(File, 0, SEEK_SET) == 0) ? Size : -1;
}

// Counts (or, with Remove, deletes) the region files of the directory.
static void ScanRegions(LodCache *Cache, bool Remove, int *OutFiles,
                        int64_t *OutBytes) {
  *OutFiles = 0;
  *OutBytes = 0;
  DIR *Dir = opendir(Cache->Directory);
  if (Dir == NULL) {
    return;
  }
  char Path[LOD_CACHE_PATH_CAPACITY];
  for (struct dirent *Entry = readdir(Dir); Entry != NULL; Entry = readdir(Dir)) {
    if (!IsCacheFile(Entry->d_name)) {
      continue;
    }
    snprintf(Path, sizeof(Path), "%s/%s", Cache->Directory, Entry->d_name);
    if (Remove) {
      (void)remove(Path);
      continue;
    }
    if (!EndsWith(Entry->d_name, REGION_SUFFIX)) {
      continue;
    }
    FILE *File = fopen(Path, "rb");
    int Level = 0;
    int RegionX = 0;
    int RegionZ = 0;
    if (File != NULL) {
      long Size = FileSize(File);
      fclose(File);
      (*OutFiles)++;
      *OutBytes += (Size > 0) ? Size : 0;
      if (ParseRegionName(Entry->d_name, &Level, &RegionX, &RegionZ)) {
        KnownAdd(Cache, Level, RegionX, RegionZ);
      }
    }
  }
  closedir(Dir);
  if (Remove) {
    pthread_mutex_lock(&Cache->KnownLock);
    for (int Idx = 0; Idx < Cache->KnownCapacity; Idx++) {
      Cache->Known[Idx] = 0U;
    }
    Cache->KnownCount = 0;
    pthread_mutex_unlock(&Cache->KnownLock);
  }
}

static void PutIdentity(const LodCache *Cache, uint8_t *Out) {
  Put32(Out, Cache->FormatVersion);
  Put32(Out + WORD_BYTES, Cache->WorldgenVersion);
}

typedef enum { META_MISSING, META_MATCH, META_MISMATCH } MetaStatus;

static MetaStatus ReadMeta(const LodCache *Cache) {
  char Path[LOD_CACHE_PATH_CAPACITY];
  snprintf(Path, sizeof(Path), "%s/%s", Cache->Directory, META_NAME);
  FILE *File = fopen(Path, "rb");
  if (File == NULL) {
    return META_MISSING;
  }
  uint8_t Meta[META_BYTES];
  size_t Read = fread(Meta, 1, sizeof(Meta), File);
  fclose(File);
  uint8_t Expected[META_BYTES] = {0};
  memcpy(Expected, META_MAGIC, MAGIC_BYTES);
  PutIdentity(Cache, Expected + IDENTITY_AT);
  Put64(Expected + META_SEED_AT, Cache->Seed);
  Put64(Expected + META_CHECKSUM_AT, Fnv(FNV_OFFSET, Expected, META_CHECKSUM_AT));
  return (Read == sizeof(Meta) && memcmp(Meta, Expected, sizeof(Meta)) == 0)
             ? META_MATCH
             : META_MISMATCH;
}

static bool WriteMeta(const LodCache *Cache) {
  uint8_t Meta[META_BYTES] = {0};
  memcpy(Meta, META_MAGIC, MAGIC_BYTES);
  PutIdentity(Cache, Meta + IDENTITY_AT);
  Put64(Meta + META_SEED_AT, Cache->Seed);
  Put64(Meta + META_CHECKSUM_AT, Fnv(FNV_OFFSET, Meta, META_CHECKSUM_AT));
  char Path[LOD_CACHE_PATH_CAPACITY];
  char Temp[LOD_CACHE_PATH_CAPACITY];
  snprintf(Path, sizeof(Path), "%s/%s", Cache->Directory, META_NAME);
  snprintf(Temp, sizeof(Temp), "%s/%s.tmp", Cache->Directory, META_NAME);
  FILE *File = fopen(Temp, "wb");
  if (File == NULL) {
    return false;
  }
  bool Written = fwrite(Meta, 1, sizeof(Meta), File) == sizeof(Meta);
  Written = (fclose(File) == 0) && Written;
  return Written && OsReplaceFile(Temp, Path);
}

LodCache *LodCacheOpen(const LodCacheConfig *Config) {
  if (Config->Directory == NULL ||
      strlen(Config->Directory) + (size_t)PATH_NAME_ROOM >= (size_t)LOD_CACHE_PATH_CAPACITY) {
    LogWarn("LOD_CACHE: directory path missing or too long");
    return NULL;
  }
  if (Config->Writable && !OsMakeDirectories(Config->Directory)) {
    LogWarn("LOD_CACHE: could not create %s; cache off", Config->Directory);
    return NULL;
  }
  LodCache *Cache = (LodCache *)calloc(1, sizeof(LodCache));
  if (Cache == NULL) {
    return NULL;
  }
  snprintf(Cache->Directory, sizeof(Cache->Directory), "%s", Config->Directory);
  Cache->Seed = Config->Seed;
  Cache->WorldgenVersion = Config->WorldgenVersion;
  Cache->FormatVersion = Config->FormatVersion;
  Cache->Writable = Config->Writable;
  Cache->Readable = true;
  for (int Idx = 0; Idx < LOCK_STRIPES; Idx++) {
    pthread_mutex_init(&Cache->Stripes[Idx], NULL);
  }
  pthread_mutex_init(&Cache->StatsLock, NULL);
  pthread_mutex_init(&Cache->KnownLock, NULL);
  Cache->Stats.Writable = Config->Writable;

  int Files = 0;
  int64_t Bytes = 0;
  ScanRegions(Cache, false, &Files, &Bytes);
  MetaStatus Meta = IsDirectory(Cache->Directory) ? ReadMeta(Cache) : META_MISSING;
  if (Meta != META_MATCH) {
    bool Stale = Meta == META_MISMATCH || Files > 0;
    if (Cache->Writable) {
      if (Stale) {
        ScanRegions(Cache, true, &Files, &Bytes);
        LogWarn("LOD_CACHE: %s belongs to another seed, generator or format; "
                "discarded its regions",
                Cache->Directory);
      }
      if (!WriteMeta(Cache)) {
        LogWarn("LOD_CACHE: could not write %s/%s; cache off", Cache->Directory,
                META_NAME);
        LodCacheClose(Cache);
        return NULL;
      }
      Files = 0;
      Bytes = 0;
    } else if (Stale) {
      Cache->Readable = false;
    }
    Cache->Stats.Invalidated = Stale;
  }
  Cache->Stats.Regions = Files;
  Cache->Stats.DiskBytes = Bytes;
  LogInfo("LOD_CACHE: %s (%s), %d regions, %.1f MB", Cache->Directory,
          Cache->Writable ? "read-write" : "read-only", Files,
          (double)Bytes / BYTES_PER_MEGABYTE);
  return Cache;
}

void LodCacheClose(LodCache *Cache) {
  if (Cache == NULL) {
    return;
  }
  for (int Idx = 0; Idx < LOCK_STRIPES; Idx++) {
    pthread_mutex_destroy(&Cache->Stripes[Idx]);
  }
  pthread_mutex_destroy(&Cache->StatsLock);
  pthread_mutex_destroy(&Cache->KnownLock);
  free(Cache->Known);
  free(Cache);
}

void LodCacheGetStats(LodCache *Cache, LodCacheStats *Out) {
  pthread_mutex_lock(&Cache->StatsLock);
  *Out = Cache->Stats;
  pthread_mutex_unlock(&Cache->StatsLock);
}

static void CountCorrupt(LodCache *Cache, const char *Path, const char *What) {
  pthread_mutex_lock(&Cache->StatsLock);
  uint64_t Seen = Cache->Stats.Corrupt++;
  pthread_mutex_unlock(&Cache->StatsLock);
  if (Seen < (uint64_t)CORRUPT_WARNINGS) {
    LogWarn("LOD_CACHE: %s: %s; ignored, it will be generated again", Path, What);
  }
}

// --- region files -----------------------------------------------------------

typedef struct {
  int Column;
  int Y;
  uint32_t Offset;
  uint32_t Size;
  uint64_t Checksum;
  const uint8_t *Data;
  // Merge order: a new record (0) replaces an old one (1) of the same key.
  int Age;
} RegionEntry;

typedef struct {
  uint32_t EntryCount;
  uint64_t GeneratedMask;
  RegionEntry *Entries;
} RegionIndex;

static bool HeaderMatches(const LodCache *Cache, const uint8_t *Header,
                          int Level, int RegionX, int RegionZ) {
  uint8_t Identity[2 * WORD_BYTES];
  PutIdentity(Cache, Identity);
  return memcmp(Header, REGION_MAGIC, MAGIC_BYTES) == 0 &&
         memcmp(Header + IDENTITY_AT, Identity, sizeof(Identity)) == 0 &&
         Get32(Header + HEADER_LEVEL_AT) == (uint32_t)Level &&
         Get64(Header + HEADER_SEED_AT) == Cache->Seed &&
         (int32_t)Get32(Header + HEADER_REGION_X_AT) == RegionX &&
         (int32_t)Get32(Header + HEADER_REGION_Z_AT) == RegionZ;
}

// Validates a region's header and index (Bytes: the header and the index, at
// least) against FileBytes, the file's size. Index->Entries is allocated.
static bool ParseIndex(const LodCache *Cache, const uint8_t *Bytes,
                       size_t Available, long FileBytes, int Level, int RegionX,
                       int RegionZ, RegionIndex *Index) {
  Index->Entries = NULL;
  if (Available < (size_t)HEADER_BYTES ||
      !HeaderMatches(Cache, Bytes, Level, RegionX, RegionZ)) {
    return false;
  }
  Index->EntryCount = Get32(Bytes + HEADER_ENTRIES_AT);
  Index->GeneratedMask = Get64(Bytes + HEADER_MASK_AT);
  size_t IndexEnd =
      (size_t)HEADER_BYTES + ((size_t)Index->EntryCount * INDEX_ENTRY_BYTES);
  if (Index->EntryCount > (uint32_t)MAX_REGION_ENTRIES || IndexEnd > Available ||
      (long)IndexEnd > FileBytes) {
    return false;
  }
  uint64_t Checksum = Fnv(FNV_OFFSET, Bytes, HEADER_CHECKSUM_AT);
  Checksum = Fnv(Checksum, Bytes + HEADER_BYTES, IndexEnd - HEADER_BYTES);
  if (Checksum != Get64(Bytes + HEADER_CHECKSUM_AT)) {
    return false;
  }
  Index->Entries = (RegionEntry *)malloc(
      sizeof(RegionEntry) * ((size_t)Index->EntryCount + 1U));
  if (Index->Entries == NULL) {
    return false;
  }
  for (uint32_t Idx = 0; Idx < Index->EntryCount; Idx++) {
    const uint8_t *At = Bytes + HEADER_BYTES + ((size_t)Idx * INDEX_ENTRY_BYTES);
    RegionEntry *Entry = &Index->Entries[Idx];
    Entry->Column = (int)Get16(At);
    Entry->Y = (int32_t)Get32(At + ENTRY_Y_AT);
    Entry->Offset = Get32(At + ENTRY_OFFSET_AT);
    Entry->Size = Get32(At + ENTRY_SIZE_AT);
    Entry->Checksum = Get64(At + ENTRY_CHECKSUM_AT);
    Entry->Data = NULL;
    Entry->Age = 1;
    if (Entry->Column >= REGION_COLUMN_COUNT || Entry->Offset < IndexEnd ||
        Entry->Size > (uint32_t)MAX_RECORD_BYTES ||
        (long)Entry->Offset + (long)Entry->Size > FileBytes) {
      free(Index->Entries);
      Index->Entries = NULL;
      return false;
    }
  }
  return true;
}

static int LocalColumn(int X, int Z) {
  int LocalX = X - (LodFloorShift(X, LOD_CACHE_REGION_SHIFT) << LOD_CACHE_REGION_SHIFT);
  int LocalZ = Z - (LodFloorShift(Z, LOD_CACHE_REGION_SHIFT) << LOD_CACHE_REGION_SHIFT);
  return (LocalX * LOD_CACHE_REGION_COLUMNS) + LocalZ;
}

static void AddReadStats(LodCache *Cache, bool Found, int Sections, size_t Bytes,
                         double Start) {
  uint64_t Micros =
      (uint64_t)((OsGetTimeSeconds() - Start) * (double)MICROS_PER_SECOND);
  pthread_mutex_lock(&Cache->StatsLock);
  Cache->Stats.ColumnReads++;
  Cache->Stats.ColumnsFound += Found ? 1U : 0U;
  Cache->Stats.SectionsRead += (uint64_t)Sections;
  Cache->Stats.BytesRead += (uint64_t)Bytes;
  Cache->Stats.ReadMicros += Micros;
  pthread_mutex_unlock(&Cache->StatsLock);
}

static void FreeEntries(LodCacheEntry *Out, int Count) {
  for (int Idx = 0; Idx < Count; Idx++) {
    LodSectionFree(&Out[Idx].Section);
  }
}

// The column's records, already located in Index and read into Buffer (which
// starts at file offset First), decoded into Out. -1 on a bad record.
static int DecodeColumn(const RegionIndex *Index, uint32_t Begin, uint32_t End,
                        const uint8_t *Buffer, uint32_t First, uint8_t *Work,
                        LodCacheEntry *Out, int Capacity) {
  int Count = 0;
  for (uint32_t Idx = Begin; Idx < End; Idx++) {
    const RegionEntry *Entry = &Index->Entries[Idx];
    const uint8_t *Record = Buffer + (Entry->Offset - First);
    if (Count >= Capacity || Fnv(FNV_OFFSET, Record, Entry->Size) != Entry->Checksum ||
        !DeserializeSection(Record, Entry->Size, Work, &Out[Count].Section)) {
      FreeEntries(Out, Count);
      return -1;
    }
    Out[Count].Y = Entry->Y;
    Count++;
  }
  return Count;
}

int LodCacheReadColumn(LodCache *Cache, int Level, int X, int Z,
                       LodCacheEntry *Out, int Capacity, bool *Generated) {
  *Generated = false;
  if (!Cache->Readable) {
    return -1;
  }
  double Start = OsGetTimeSeconds();
  int RegionX = LodFloorShift(X, LOD_CACHE_REGION_SHIFT);
  int RegionZ = LodFloorShift(Z, LOD_CACHE_REGION_SHIFT);
  int Column = LocalColumn(X, Z);
  if (!KnownHas(Cache, Level, RegionX, RegionZ)) {
    AddReadStats(Cache, false, 0, 0U, Start);
    return -1;
  }
  char Path[LOD_CACHE_PATH_CAPACITY];
  RegionPath(Cache, Level, RegionX, RegionZ, REGION_SUFFIX, Path);

  pthread_mutex_t *Lock = StripeOf(Cache, Level, RegionX, RegionZ);
  pthread_mutex_lock(Lock);
  FILE *File = fopen(Path, "rb");
  if (File == NULL) {
    pthread_mutex_unlock(Lock);
    AddReadStats(Cache, false, 0, 0U, Start);
    return -1;
  }
  long FileBytes = FileSize(File);
  uint8_t Header[HEADER_BYTES];
  RegionIndex Index = {0};
  uint8_t *Bytes = NULL;
  uint8_t *Buffer = NULL;
  uint8_t *Work = NULL;
  int Result = -1;
  size_t ReadBytes = 0;
  const char *Problem = "unreadable header or index";
  if (FileBytes >= HEADER_BYTES && fread(Header, 1, sizeof(Header), File) == sizeof(Header)) {
    uint32_t Entries = Get32(Header + HEADER_ENTRIES_AT);
    size_t IndexBytes = (Entries <= (uint32_t)MAX_REGION_ENTRIES)
                            ? (size_t)Entries * INDEX_ENTRY_BYTES
                            : 0U;
    Bytes = (uint8_t *)malloc((size_t)HEADER_BYTES + IndexBytes);
    if (Bytes != NULL) {
      memcpy(Bytes, Header, sizeof(Header));
      size_t Got = fread(Bytes + HEADER_BYTES, 1, IndexBytes, File);
      if (Got == IndexBytes &&
          ParseIndex(Cache, Bytes, HEADER_BYTES + IndexBytes, FileBytes, Level,
                     RegionX, RegionZ, &Index)) {
        Problem = NULL;
      }
    }
  }
  if (Problem == NULL) {
    *Generated = ((Index.GeneratedMask >> (unsigned)Column) & 1U) != 0U;
    uint32_t Begin = 0;
    while (Begin < Index.EntryCount && Index.Entries[Begin].Column != Column) {
      Begin++;
    }
    uint32_t End = Begin;
    while (End < Index.EntryCount && Index.Entries[End].Column == Column) {
      End++;
    }
    Result = 0;
    if (End > Begin) {
      uint32_t First = Index.Entries[Begin].Offset;
      uint32_t Last = First;
      for (uint32_t Idx = Begin; Idx < End; Idx++) {
        First = (Index.Entries[Idx].Offset < First) ? Index.Entries[Idx].Offset : First;
        uint32_t Tail = Index.Entries[Idx].Offset + Index.Entries[Idx].Size;
        Last = (Tail > Last) ? Tail : Last;
      }
      ReadBytes = (size_t)(Last - First);
      Buffer = (ReadBytes > 0U) ? (uint8_t *)malloc(ReadBytes) : NULL;
      Work = (uint8_t *)malloc((size_t)LOD_DENSE_BYTES);
      Result = -1;
      Problem = "unreadable record";
      if (Buffer != NULL && Work != NULL && fseek(File, (long)First, SEEK_SET) == 0 &&
          fread(Buffer, 1, ReadBytes, File) == ReadBytes) {
        Result = DecodeColumn(&Index, Begin, End, Buffer, First, Work, Out, Capacity);
        Problem = (Result < 0) ? "record failed its checksum or bounds" : NULL;
      }
    }
  }
  fclose(File);
  pthread_mutex_unlock(Lock);
  if (Problem != NULL) {
    CountCorrupt(Cache, Path, Problem);
    *Generated = false;
    Result = -1;
  }
  free(Index.Entries);
  free(Bytes);
  free(Buffer);
  free(Work);
  AddReadStats(Cache, Result >= 0, (Result > 0) ? Result : 0, ReadBytes, Start);
  return Result;
}

static int CompareEntries(const void *Left, const void *Right) {
  const RegionEntry *A = (const RegionEntry *)Left;
  const RegionEntry *B = (const RegionEntry *)Right;
  if (A->Column != B->Column) {
    return (A->Column < B->Column) ? -1 : 1;
  }
  if (A->Y != B->Y) {
    return (A->Y < B->Y) ? -1 : 1;
  }
  return (A->Age < B->Age) ? -1 : (A->Age > B->Age);
}

static int CompareItems(const void *Left, const void *Right) {
  const LodCacheItem *A = (const LodCacheItem *)Left;
  const LodCacheItem *B = (const LodCacheItem *)Right;
  int KeysA[ITEM_KEYS] = {A->Level, LodFloorShift(A->X, LOD_CACHE_REGION_SHIFT),
                  LodFloorShift(A->Z, LOD_CACHE_REGION_SHIFT), LocalColumn(A->X, A->Z),
                  A->Y};
  int KeysB[ITEM_KEYS] = {B->Level, LodFloorShift(B->X, LOD_CACHE_REGION_SHIFT),
                  LodFloorShift(B->Z, LOD_CACHE_REGION_SHIFT), LocalColumn(B->X, B->Z),
                  B->Y};
  for (int Key = 0; Key < ITEM_KEYS; Key++) {
    if (KeysA[Key] != KeysB[Key]) {
      return (KeysA[Key] < KeysB[Key]) ? -1 : 1;
    }
  }
  return 0;
}

// The records of the region file as it is, checksums verified (bad ones are
// dropped). *OutFile owns the bytes the entries point into.
static int LoadOldEntries(LodCache *Cache, const char *Path, int Level,
                          int RegionX, int RegionZ, uint8_t **OutFile,
                          long *OutBytes, RegionEntry *Entries, uint64_t *Mask) {
  *OutFile = NULL;
  *OutBytes = 0;
  int Size = 0;
  FILE *Probe = fopen(Path, "rb");
  if (Probe == NULL) {
    return 0;
  }
  fclose(Probe);
  uint8_t *File = ReadBinaryFile(Path, &Size);
  RegionIndex Index = {0};
  if (File == NULL || !ParseIndex(Cache, File, (size_t)Size, Size, Level, RegionX,
                                  RegionZ, &Index)) {
    CountCorrupt(Cache, Path, "invalid region rewritten from scratch");
    free(File);
    *OutBytes = (Size > 0) ? Size : 0;
    return 0;
  }
  int Kept = 0;
  for (uint32_t Idx = 0; Idx < Index.EntryCount; Idx++) {
    RegionEntry Entry = Index.Entries[Idx];
    Entry.Data = File + Entry.Offset;
    if (Fnv(FNV_OFFSET, Entry.Data, Entry.Size) != Entry.Checksum) {
      CountCorrupt(Cache, Path, "record failed its checksum, dropped");
      continue;
    }
    Entries[Kept++] = Entry;
  }
  *Mask = Index.GeneratedMask;
  free(Index.Entries);
  *OutFile = File;
  *OutBytes = Size;
  return Kept;
}

typedef struct {
  uint8_t *Bytes;
  size_t Used;
  size_t Capacity;
} ByteBuffer;

static bool Reserve(ByteBuffer *Buffer, size_t More) {
  if (Buffer->Used + More <= Buffer->Capacity) {
    return true;
  }
  size_t Capacity = (Buffer->Capacity > 0U) ? Buffer->Capacity : INITIAL_OUTPUT_BYTES;
  while (Capacity < Buffer->Used + More) {
    Capacity *= 2U;
  }
  uint8_t *Grown = (uint8_t *)realloc(Buffer->Bytes, Capacity);
  if (Grown == NULL) {
    return false;
  }
  Buffer->Bytes = Grown;
  Buffer->Capacity = Capacity;
  return true;
}

typedef struct {
  uint64_t Uniform;
  uint64_t Rle;
  uint64_t Raw;
  uint64_t RecordBytes;
  uint64_t MemoryBytes;
} SerialCounts;

// Serialises one region's new records into Records; their entries point at
// offsets inside it until the file is laid out.
static bool SerializeItems(const LodCacheItem *Items, int Count, ByteBuffer *Records,
                           RegionEntry *Entries, uint64_t *Mask, SerialCounts *Counts) {
  uint8_t *Work = (uint8_t *)malloc((size_t)LOD_DENSE_BYTES);
  if (Work == NULL) {
    return false;
  }
  bool Passed = true;
  for (int Idx = 0; Idx < Count && Passed; Idx++) {
    const LodCacheItem *Item = &Items[Idx];
    Passed = Reserve(Records, MAX_RECORD_BYTES);
    if (!Passed) {
      break;
    }
    RecordKind Kind = RECORD_RAW;
    size_t Size =
        SerializeSection(Item->Section, Records->Bytes + Records->Used, Work, &Kind);
    int Column = LocalColumn(Item->X, Item->Z);
    Entries[Idx] = (RegionEntry){.Column = Column,
                                 .Y = Item->Y,
                                 .Offset = (uint32_t)Records->Used,
                                 .Size = (uint32_t)Size,
                                 .Age = 0};
    Records->Used += Size;
    if (Item->Section->GeneratedColumn) {
      *Mask |= (uint64_t)1U << (unsigned)Column;
    }
    Counts->Uniform += (Kind == RECORD_UNIFORM) ? 1U : 0U;
    Counts->Rle += (Kind == RECORD_RLE) ? 1U : 0U;
    Counts->Raw += (Kind == RECORD_RAW) ? 1U : 0U;
    Counts->RecordBytes += Size;
    Counts->MemoryBytes += sizeof(LodSection) + LodSectionHeapBytes(Item->Section);
  }
  free(Work);
  return Passed;
}

// Header, index and records of the merged region, in index order.
static bool LayOut(const LodCache *Cache, int Level, int RegionX, int RegionZ,
                   RegionEntry *Entries, int Count, uint64_t Mask, ByteBuffer *Out) {
  size_t IndexEnd = (size_t)HEADER_BYTES + ((size_t)Count * INDEX_ENTRY_BYTES);
  size_t Total = IndexEnd;
  for (int Idx = 0; Idx < Count; Idx++) {
    Total += Entries[Idx].Size;
  }
  if (!Reserve(Out, Total)) {
    return false;
  }
  uint8_t *Bytes = Out->Bytes;
  memcpy(Bytes, REGION_MAGIC, MAGIC_BYTES);
  PutIdentity(Cache, Bytes + IDENTITY_AT);
  Put32(Bytes + HEADER_LEVEL_AT, (uint32_t)Level);
  Put64(Bytes + HEADER_SEED_AT, Cache->Seed);
  Put32(Bytes + HEADER_REGION_X_AT, (uint32_t)RegionX);
  Put32(Bytes + HEADER_REGION_Z_AT, (uint32_t)RegionZ);
  Put32(Bytes + HEADER_ENTRIES_AT, (uint32_t)Count);
  Put32(Bytes + HEADER_RESERVED_AT, 0U);
  Put64(Bytes + HEADER_MASK_AT, Mask);
  size_t Offset = IndexEnd;
  for (int Idx = 0; Idx < Count; Idx++) {
    RegionEntry *Entry = &Entries[Idx];
    memcpy(Bytes + Offset, Entry->Data, Entry->Size);
    uint8_t *At = Bytes + HEADER_BYTES + ((size_t)Idx * INDEX_ENTRY_BYTES);
    Put16(At, (uint32_t)Entry->Column);
    Put16(At + ENTRY_RESERVED_AT, 0U);
    Put32(At + ENTRY_Y_AT, (uint32_t)Entry->Y);
    Put32(At + ENTRY_OFFSET_AT, (uint32_t)Offset);
    Put32(At + ENTRY_SIZE_AT, Entry->Size);
    Put64(At + ENTRY_CHECKSUM_AT, Fnv(FNV_OFFSET, Bytes + Offset, Entry->Size));
    Offset += Entry->Size;
  }
  uint64_t Checksum = Fnv(FNV_OFFSET, Bytes, HEADER_CHECKSUM_AT);
  Checksum = Fnv(Checksum, Bytes + HEADER_BYTES, IndexEnd - HEADER_BYTES);
  Put64(Bytes + HEADER_CHECKSUM_AT, Checksum);
  Out->Used = Total;
  return true;
}

static bool WriteFileReplacing(const char *Temp, const char *Path,
                               const ByteBuffer *Out) {
  FILE *File = fopen(Temp, "wb");
  if (File == NULL) {
    return false;
  }
  bool Written = fwrite(Out->Bytes, 1, Out->Used, File) == Out->Used;
  Written = (fclose(File) == 0) && Written;
  // A sync client or virus scanner briefly holding the old file refuses the
  // replace; it is retried for a few milliseconds before the write fails.
  bool Replaced = false;
  for (int Attempt = 0; Written && !Replaced && Attempt < REPLACE_ATTEMPTS; Attempt++) {
    if (Attempt > 0) {
      OsSleepMillis(REPLACE_WAIT_MS * Attempt);
    }
    Replaced = OsReplaceFile(Temp, Path);
  }
  if (!Replaced) {
    (void)remove(Temp);
  }
  return Replaced;
}

static bool WriteRegion(LodCache *Cache, const LodCacheItem *Items, int Count) {
  double Start = OsGetTimeSeconds();
  int Level = Items[0].Level;
  int RegionX = LodFloorShift(Items[0].X, LOD_CACHE_REGION_SHIFT);
  int RegionZ = LodFloorShift(Items[0].Z, LOD_CACHE_REGION_SHIFT);
  char Path[LOD_CACHE_PATH_CAPACITY];
  char Temp[LOD_CACHE_PATH_CAPACITY];
  RegionPath(Cache, Level, RegionX, RegionZ, REGION_SUFFIX, Path);
  RegionPath(Cache, Level, RegionX, RegionZ, TEMP_SUFFIX, Temp);

  RegionEntry *Entries = (RegionEntry *)malloc(
      sizeof(RegionEntry) * ((size_t)MAX_REGION_ENTRIES + (size_t)Count));
  ByteBuffer Records = {0};
  ByteBuffer Out = {0};
  SerialCounts Counts = {0};
  uint64_t Mask = 0;
  bool Passed = Entries != NULL &&
                SerializeItems(Items, Count, &Records, Entries, &Mask, &Counts);
  for (int Idx = 0; Passed && Idx < Count; Idx++) {
    Entries[Idx].Data = Records.Bytes + Entries[Idx].Offset;
  }

  pthread_mutex_t *Lock = StripeOf(Cache, Level, RegionX, RegionZ);
  pthread_mutex_lock(Lock);
  uint8_t *OldFile = NULL;
  long OldBytes = 0;
  int Merged = Count;
  if (Passed) {
    uint64_t OldMask = 0;
    Merged += LoadOldEntries(Cache, Path, Level, RegionX, RegionZ, &OldFile,
                             &OldBytes, Entries + Count, &OldMask);
    Mask |= OldMask;
    qsort(Entries, (size_t)Merged, sizeof(RegionEntry), CompareEntries);
    int Unique = 0;
    for (int Idx = 0; Idx < Merged; Idx++) {
      if (Unique > 0 && Entries[Unique - 1].Column == Entries[Idx].Column &&
          Entries[Unique - 1].Y == Entries[Idx].Y) {
        continue;
      }
      Entries[Unique++] = Entries[Idx];
    }
    Merged = Unique;
    Passed = Merged <= MAX_REGION_ENTRIES &&
             LayOut(Cache, Level, RegionX, RegionZ, Entries, Merged, Mask, &Out) &&
             WriteFileReplacing(Temp, Path, &Out);
  }
  pthread_mutex_unlock(Lock);

  uint64_t Micros =
      (uint64_t)((OsGetTimeSeconds() - Start) * (double)MICROS_PER_SECOND);
  pthread_mutex_lock(&Cache->StatsLock);
  if (Passed) {
    Cache->Stats.Regions += (OldFile == NULL && OldBytes == 0) ? 1 : 0;
    Cache->Stats.DiskBytes += (int64_t)Out.Used - (int64_t)OldBytes;
    Cache->Stats.SectionsWritten += (uint64_t)Count;
    Cache->Stats.RecordBytesWritten += Counts.RecordBytes;
    Cache->Stats.MemoryBytesWritten += Counts.MemoryBytes;
    Cache->Stats.UniformSections += Counts.Uniform;
    Cache->Stats.RleSections += Counts.Rle;
    Cache->Stats.RawSections += Counts.Raw;
    Cache->Stats.RegionWrites++;
    Cache->Stats.FileBytesWritten += Out.Used;
  }
  Cache->Stats.WriteMicros += Micros;
  pthread_mutex_unlock(&Cache->StatsLock);
  if (Passed) {
    KnownAdd(Cache, Level, RegionX, RegionZ);
  } else {
    LogWarn("LOD_CACHE: could not write %s", Path);
  }
  free(OldFile);
  free(Records.Bytes);
  free(Out.Bytes);
  free(Entries);
  return Passed;
}

bool LodCacheWrite(LodCache *Cache, LodCacheItem *Items, int Count) {
  if (!Cache->Writable) {
    return false;
  }
  qsort(Items, (size_t)Count, sizeof(LodCacheItem), CompareItems);
  bool Passed = true;
  int Start = 0;
  while (Start < Count) {
    int End = Start + 1;
    while (End < Count && Items[End].Level == Items[Start].Level &&
           LodFloorShift(Items[End].X, LOD_CACHE_REGION_SHIFT) ==
               LodFloorShift(Items[Start].X, LOD_CACHE_REGION_SHIFT) &&
           LodFloorShift(Items[End].Z, LOD_CACHE_REGION_SHIFT) ==
               LodFloorShift(Items[Start].Z, LOD_CACHE_REGION_SHIFT)) {
      End++;
    }
    Passed = WriteRegion(Cache, Items + Start, End - Start) && Passed;
    Start = End;
  }
  return Passed;
}
