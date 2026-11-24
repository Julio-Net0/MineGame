#ifndef WORLD_LOD_LOD_CACHE_H
#define WORLD_LOD_LOD_CACHE_H

#include "world/lod/lod_section.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The LOD hierarchy's disk cache: whole sections, with their provenance, kept
// between sessions so the horizon reappears without being generated again and
// real data (built or edited terrain) survives far from the loaded chunks.
//
// Files live in one directory: meta.bin names the format, the generator version
// and the seed the cache belongs to, and each region file holds the sections of
// LOD_CACHE_REGION_COLUMNS^2 section columns of one level
// ("l<level>.<rx>.<rz>.lod"). A region file is a header (the same identity as
// meta.bin plus a bitmask of the columns generated whole), an index sorted by
// column and height with a checksum per record, then the records. A record is
// the section's provenance and storage mode, its palette and its voxels or
// palette indices, run-length encoded in height-major order when that is
// smaller and stored raw otherwise.
//
// Regions are rewritten whole: into a temporary file, then moved over the old
// one, so a crash leaves the old region or the new one. Anything that fails
// validation (identity, bounds, checksums) is ignored with a warning and the
// column is generated again; the next write of that region drops it.
//
// Thread-safe: reads and writes of one region are serialised by a lock per
// region stripe. No globals; nothing touches the disk outside the calls below.

enum {
  LOD_CACHE_FORMAT_VERSION = 1,
  LOD_CACHE_REGION_SHIFT = 3,
  LOD_CACHE_REGION_COLUMNS = 1 << LOD_CACHE_REGION_SHIFT,
  // Sections one column may hold: the world's height range spans 36 level-1
  // sections.
  LOD_CACHE_MAX_COLUMN_SECTIONS = 48,
  LOD_CACHE_PATH_CAPACITY = 512
};

typedef struct LodCache LodCache;

typedef struct {
  const char *Directory;
  uint64_t Seed;
  // The identity the cache must match; normally WORLDGEN_VERSION and
  // LOD_CACHE_FORMAT_VERSION (tests vary them).
  uint32_t WorldgenVersion;
  uint32_t FormatVersion;
  // A cache that is not writable never creates, changes or removes a file;
  // an identity mismatch then just makes every read miss.
  bool Writable;
} LodCacheConfig;

// NULL when the directory cannot be created (writable) or memory runs out.
// A writable cache whose meta.bin is missing or names another identity has its
// region files removed and meta.bin rewritten.
LodCache *LodCacheOpen(const LodCacheConfig *Config);
void LodCacheClose(LodCache *Cache);

// One cached section of a column; Section holds the content and its
// PresentCells / IngestedCells, and is the caller's to free.
typedef struct {
  int Y;
  LodSection Section;
} LodCacheEntry;

// Reads every cached section of the level-Level column (X, Z), lowest first,
// into Out. Returns how many, or -1 when the region is missing or the column
// fails validation (Out then holds nothing). *Generated says whether the column
// was cached as generated whole.
int LodCacheReadColumn(LodCache *Cache, int Level, int X, int Z,
                       LodCacheEntry *Out, int Capacity, bool *Generated);

// One section to save; its provenance and GeneratedColumn are written with it.
typedef struct {
  int Level;
  int X;
  int Y;
  int Z;
  const LodSection *Section;
} LodCacheItem;

// Saves Items (each key once), replacing the cached copy of each, region by
// region (reorders Items). A column's sections are written in one region write, so a crash
// never leaves part of a call's column. False when not writable or any region
// failed.
bool LodCacheWrite(LodCache *Cache, LodCacheItem *Items, int Count);

typedef struct {
  bool Writable;
  // The identity did not match on open (or was missing next to region files)
  // and the old regions were discarded.
  bool Invalidated;
  int Regions;
  int64_t DiskBytes;
  uint64_t ColumnReads;
  uint64_t ColumnsFound;
  uint64_t SectionsRead;
  uint64_t BytesRead;
  uint64_t ReadMicros;
  uint64_t SectionsWritten;
  // Record bytes of the written sections, and what the same sections take in
  // memory (struct plus storage).
  uint64_t RecordBytesWritten;
  uint64_t MemoryBytesWritten;
  uint64_t RegionWrites;
  uint64_t FileBytesWritten;
  uint64_t WriteMicros;
  uint64_t UniformSections;
  uint64_t RleSections;
  uint64_t RawSections;
  // Regions and records rejected by validation.
  uint64_t Corrupt;
} LodCacheStats;

void LodCacheGetStats(LodCache *Cache, LodCacheStats *Out);

#endif
