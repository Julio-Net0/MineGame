#ifndef WORLD_LOD_LOD_SECTION_H
#define WORLD_LOD_LOD_SECTION_H

#include "world/lod/lod_voxel.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A cube of 16^3 LOD voxels. At level L each voxel stands for a 2^L block cube,
// so the section spans 16 * 2^L blocks: level 1 covers 2x2x2 chunks, level 7
// covers 128 chunks per axis, which is what a 2048-chunk view distance needs.
// Level 0 is never stored; it is the world's own chunks.
//
// Voxels are indexed [X][Y][Z] with Z contiguous, like chunk storage. Storage
// takes the smallest of four forms that holds the content exactly:
//   UNIFORM  one voxel value, no heap storage
//   PACKED4  <= 16 distinct values, 4-bit palette indices (2 KB + palette)
//   PACKED8  <= 256 distinct values, 8-bit palette indices (4 KB + palette)
//   DENSE    raw voxels (16 KB)
//
// Provenance is tracked per 4^3 cell (64 cells, one bit each): which cells were
// ever written, and which of those derive from real chunks rather than from the
// synthetic generator. Real data always wins over synthetic data.

enum {
  LOD_SECTION_SIZE = 16,
  LOD_SECTION_VOLUME = LOD_SECTION_SIZE * LOD_SECTION_SIZE * LOD_SECTION_SIZE,
  LOD_MAX_LEVEL = 7,
  LOD_CELL_SIZE = 4,
  LOD_CELLS_PER_AXIS = LOD_SECTION_SIZE / LOD_CELL_SIZE,
  LOD_CELL_COUNT = LOD_CELLS_PER_AXIS * LOD_CELLS_PER_AXIS * LOD_CELLS_PER_AXIS,
  LOD_PACKED4_MAX_PALETTE = 16,
  LOD_PACKED8_MAX_PALETTE = 256
};

typedef enum {
  LOD_STORAGE_UNIFORM = 0,
  LOD_STORAGE_PACKED4,
  LOD_STORAGE_PACKED8,
  LOD_STORAGE_DENSE,
  LOD_STORAGE_COUNT
} LodStorageMode;

typedef enum {
  LOD_ORIGIN_SYNTHETIC = 0,
  LOD_ORIGIN_INGESTED
} LodOrigin;

typedef struct {
  LodStorageMode Mode;
  LodVoxel Uniform;
  int PaletteCount;
  // One allocation holding the palette followed by the indices (packed modes)
  // or the raw voxels (dense); NULL for a uniform section.
  void *Storage;
  size_t StorageBytes;

  uint64_t PresentCells;
  uint64_t IngestedCells;
  // Bumped on every content change; meshing compares it with the revision its
  // mesh was built from.
  uint32_t Revision;
  bool Dirty;
  // Changed since its parent was last rebuilt from it; owned by the store.
  bool PendingPropagation;
  // Held DENSE while writes keep arriving, waiting to be compacted; owned by
  // the store.
  bool Hot;
} LodSection;

static inline int LodVoxelIndex(int X, int Y, int Z) {
  return (X * LOD_SECTION_SIZE * LOD_SECTION_SIZE) + (Y * LOD_SECTION_SIZE) + Z;
}

static inline int LodCellIndex(int CellX, int CellY, int CellZ) {
  return CellX + (CellY * LOD_CELLS_PER_AXIS) +
         (CellZ * LOD_CELLS_PER_AXIS * LOD_CELLS_PER_AXIS);
}

static inline uint64_t LodCellBit(int CellX, int CellY, int CellZ) {
  return (uint64_t)1U << (unsigned)LodCellIndex(CellX, CellY, CellZ);
}

// All air, nothing present, no storage.
void LodSectionInit(LodSection *Section);
void LodSectionFree(LodSection *Section);

LodVoxel LodSectionGet(const LodSection *Section, int X, int Y, int Z);
void LodSectionDecode(const LodSection *Section,
                      LodVoxel Out[LOD_SECTION_VOLUME]);

// Palette-building workspace (about 40 KB). A long-lived owner keeps one so
// encoding allocates nothing but the section's own storage.
typedef struct LodEncodeScratch LodEncodeScratch;
LodEncodeScratch *LodEncodeScratchCreate(void);
void LodEncodeScratchFree(LodEncodeScratch *Scratch);
size_t LodEncodeScratchBytes(void);

// Replaces the content with Voxels in the smallest storage form. On allocation
// failure returns false and leaves the section exactly as it was. Provenance,
// revision and dirty state are the caller's.
bool LodSectionEncodeWithScratch(LodSection *Section,
                                 const LodVoxel Voxels[LOD_SECTION_VOLUME],
                                 LodEncodeScratch *Scratch);

// The same with a temporary workspace, for callers outside the hot path.
bool LodSectionEncode(LodSection *Section,
                      const LodVoxel Voxels[LOD_SECTION_VOLUME]);

enum {
  LOD_DENSE_BYTES = (int)(sizeof(LodVoxel) * LOD_SECTION_VOLUME)
};

// The raw voxels of a DENSE section, writable in place; NULL otherwise.
LodVoxel *LodSectionDenseVoxels(LodSection *Section);

// Makes Buffer (LOD_DENSE_BYTES, already holding the section's new content)
// its DENSE storage, freeing what it held before.
void LodSectionAdoptDense(LodSection *Section, LodVoxel *Buffer);

// Moves Source's content (storage form and voxels) into Section, freeing what
// Section held; Source is left empty. Provenance and revisions are the
// caller's.
void LodSectionAdoptContent(LodSection *Section, LodSection *Source);

// Re-encodes a DENSE section in its smallest form, exactly as LodSectionEncode
// would from the same voxels. When the dense buffer stops being used it is
// handed back in *OutReleased (else NULL) for the caller to reuse or free. On
// allocation failure returns false and leaves the section as it was.
bool LodSectionCompactDense(LodSection *Section, LodEncodeScratch *Scratch,
                            LodVoxel **OutReleased);

// Heap bytes the content occupies (zero when uniform).
size_t LodSectionHeapBytes(const LodSection *Section);

// FNV-1a over the decoded voxels, independent of the storage form.
uint64_t LodSectionContentHash(const LodSection *Section, uint64_t Seed);

#endif
