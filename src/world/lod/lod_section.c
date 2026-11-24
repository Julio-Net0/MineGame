#include "world/lod/lod_section.h"

#include <stdlib.h>
#include <string.h>

enum {
  // Open-addressing table for palette building: a power of two at least twice
  // the section volume, so a full section of distinct values stays sparse.
  PALETTE_TABLE_BITS = 13,
  PALETTE_TABLE_SIZE = 1 << PALETTE_TABLE_BITS,
  PALETTE_EMPTY = 0xFFFF,
  NIBBLE_BITS = 4,
  NIBBLE_MASK = 0xF,
  BYTE_BITS = 8,
  BYTE_MASK = 0xFF,
  VOXEL_BITS = 32
};

#define PALETTE_HASH_MULTIPLIER 0x9E3779B1U
#define FNV_PRIME 0x100000001B3ULL

struct LodEncodeScratch {
  uint16_t Slots[PALETTE_TABLE_SIZE];
  LodVoxel Palette[LOD_SECTION_VOLUME];
  uint16_t Indices[LOD_SECTION_VOLUME];
  int Count;
};

LodEncodeScratch *LodEncodeScratchCreate(void) {
  return (LodEncodeScratch *)malloc(sizeof(LodEncodeScratch));
}

void LodEncodeScratchFree(LodEncodeScratch *Scratch) { free(Scratch); }

size_t LodEncodeScratchBytes(void) { return sizeof(LodEncodeScratch); }

void LodSectionInit(LodSection *Section) {
  *Section = (LodSection){.Mode = LOD_STORAGE_UNIFORM, .Uniform = LOD_VOXEL_AIR};
}

void LodSectionFree(LodSection *Section) {
  free(Section->Storage);
  LodSectionInit(Section);
}

static const LodVoxel *SectionPalette(const LodSection *Section) {
  return (const LodVoxel *)Section->Storage;
}

static const uint8_t *SectionIndices(const LodSection *Section) {
  return (const uint8_t *)Section->Storage +
         ((size_t)Section->PaletteCount * sizeof(LodVoxel));
}

static LodVoxel GetByIndex(const LodSection *Section, int Index) {
  switch (Section->Mode) {
  case LOD_STORAGE_PACKED4: {
    unsigned Byte = SectionIndices(Section)[Index >> 1];
    unsigned Shift = ((unsigned)Index & 1U) * NIBBLE_BITS;
    return SectionPalette(Section)[(Byte >> Shift) & NIBBLE_MASK];
  }
  case LOD_STORAGE_PACKED8:
    return SectionPalette(Section)[SectionIndices(Section)[Index]];
  case LOD_STORAGE_DENSE:
    return ((const LodVoxel *)Section->Storage)[Index];
  case LOD_STORAGE_UNIFORM:
  default:
    return Section->Uniform;
  }
}

LodVoxel LodSectionGet(const LodSection *Section, int X, int Y, int Z) {
  return GetByIndex(Section, LodVoxelIndex(X, Y, Z));
}

void LodSectionDecode(const LodSection *Section,
                      LodVoxel Out[LOD_SECTION_VOLUME]) {
  if (Section->Mode == LOD_STORAGE_UNIFORM) {
    for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
      Out[Idx] = Section->Uniform;
    }
    return;
  }
  if (Section->Mode == LOD_STORAGE_DENSE) {
    memcpy(Out, Section->Storage, sizeof(LodVoxel) * LOD_SECTION_VOLUME);
    return;
  }
  const LodVoxel *Palette = SectionPalette(Section);
  const uint8_t *Indices = SectionIndices(Section);
  if (Section->Mode == LOD_STORAGE_PACKED8) {
    for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
      Out[Idx] = Palette[Indices[Idx]];
    }
    return;
  }
  for (int Even = 0; Even < LOD_SECTION_VOLUME; Even += 2) {
    unsigned Pair = Indices[Even >> 1];
    Out[Even] = Palette[Pair & NIBBLE_MASK];
    Out[Even + 1] = Palette[Pair >> NIBBLE_BITS];
  }
}

// Palette in order of first occurrence, so equal content always encodes to
// equal bytes.
static void BuildPalette(const LodVoxel *Voxels, LodEncodeScratch *Build) {
  // Every byte 0xFF makes every slot PALETTE_EMPTY.
  memset(Build->Slots, BYTE_MASK, sizeof(Build->Slots));
  Build->Count = 0;
  // Runs of equal voxels are the rule along Z, so the previous voxel's entry
  // answers most lookups without hashing.
  LodVoxel Previous = 0;
  uint16_t PreviousEntry = PALETTE_EMPTY;
  for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
    LodVoxel Value = Voxels[Idx];
    if (PreviousEntry != PALETTE_EMPTY && Value == Previous) {
      Build->Indices[Idx] = PreviousEntry;
      continue;
    }
    unsigned Slot = (Value * PALETTE_HASH_MULTIPLIER) >>
                    (unsigned)(VOXEL_BITS - PALETTE_TABLE_BITS);
    for (;;) {
      uint16_t Entry = Build->Slots[Slot];
      if (Entry == PALETTE_EMPTY) {
        Build->Slots[Slot] = (uint16_t)Build->Count;
        Build->Palette[Build->Count] = Value;
        Build->Indices[Idx] = (uint16_t)Build->Count;
        Build->Count++;
        break;
      }
      if (Build->Palette[Entry] == Value) {
        Build->Indices[Idx] = Entry;
        break;
      }
      Slot = (Slot + 1U) & (unsigned)(PALETTE_TABLE_SIZE - 1);
    }
    Previous = Value;
    PreviousEntry = Build->Indices[Idx];
  }
}

static void *EncodePacked(const LodEncodeScratch *Build, bool FourBit,
                          size_t *OutBytes) {
  size_t PaletteBytes = (size_t)Build->Count * sizeof(LodVoxel);
  size_t IndexBytes = FourBit ? (size_t)LOD_SECTION_VOLUME / 2
                              : (size_t)LOD_SECTION_VOLUME;
  uint8_t *Block = (uint8_t *)malloc(PaletteBytes + IndexBytes);
  if (Block == NULL) {
    return NULL;
  }
  memcpy(Block, Build->Palette, PaletteBytes);
  uint8_t *Indices = Block + PaletteBytes;
  if (FourBit) {
    memset(Indices, 0, IndexBytes);
    for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
      unsigned Shift = ((unsigned)Idx & 1U) * NIBBLE_BITS;
      Indices[Idx >> 1] |= (uint8_t)(Build->Indices[Idx] << Shift);
    }
  } else {
    for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
      Indices[Idx] = (uint8_t)Build->Indices[Idx];
    }
  }
  *OutBytes = PaletteBytes + IndexBytes;
  return Block;
}

static bool AllEqual(const LodVoxel *Voxels) {
  for (int Idx = 1; Idx < LOD_SECTION_VOLUME; Idx++) {
    if (Voxels[Idx] != Voxels[0]) {
      return false;
    }
  }
  return true;
}

bool LodSectionEncodeWithScratch(LodSection *Section,
                                 const LodVoxel Voxels[LOD_SECTION_VOLUME],
                                 LodEncodeScratch *Build) {
  // Most sections of a load are all air or all rock; one compare pass settles
  // them without building a palette.
  if (AllEqual(Voxels)) {
    free(Section->Storage);
    Section->Mode = LOD_STORAGE_UNIFORM;
    Section->Uniform = Voxels[0];
    Section->PaletteCount = 0;
    Section->Storage = NULL;
    Section->StorageBytes = 0;
    return true;
  }
  BuildPalette(Voxels, Build);

  LodStorageMode Mode = LOD_STORAGE_DENSE;
  void *Storage = NULL;
  size_t Bytes = 0;
  int PaletteCount = 0;
  if (Build->Count <= LOD_PACKED4_MAX_PALETTE) {
    Mode = LOD_STORAGE_PACKED4;
    PaletteCount = Build->Count;
    Storage = EncodePacked(Build, true, &Bytes);
  } else if (Build->Count <= LOD_PACKED8_MAX_PALETTE) {
    Mode = LOD_STORAGE_PACKED8;
    PaletteCount = Build->Count;
    Storage = EncodePacked(Build, false, &Bytes);
  } else {
    Bytes = sizeof(LodVoxel) * LOD_SECTION_VOLUME;
    Storage = malloc(Bytes);
    if (Storage != NULL) {
      memcpy(Storage, Voxels, Bytes);
    }
  }
  if (Storage == NULL) {
    return false;
  }

  free(Section->Storage);
  Section->Mode = Mode;
  Section->Uniform = LOD_VOXEL_AIR;
  Section->PaletteCount = PaletteCount;
  Section->Storage = Storage;
  Section->StorageBytes = Bytes;
  return true;
}

LodVoxel *LodSectionDenseVoxels(LodSection *Section) {
  return (Section->Mode == LOD_STORAGE_DENSE) ? (LodVoxel *)Section->Storage
                                              : NULL;
}

void LodSectionAdoptDense(LodSection *Section, LodVoxel *Buffer) {
  if (Section->Storage != (void *)Buffer) {
    free(Section->Storage);
  }
  Section->Mode = LOD_STORAGE_DENSE;
  Section->Uniform = LOD_VOXEL_AIR;
  Section->PaletteCount = 0;
  Section->Storage = Buffer;
  Section->StorageBytes = LOD_DENSE_BYTES;
}

void LodSectionAdoptContent(LodSection *Section, LodSection *Source) {
  free(Section->Storage);
  Section->Mode = Source->Mode;
  Section->Uniform = Source->Uniform;
  Section->PaletteCount = Source->PaletteCount;
  Section->Storage = Source->Storage;
  Section->StorageBytes = Source->StorageBytes;
  Source->Storage = NULL;
  LodSectionInit(Source);
}

bool LodSectionCopy(LodSection *Out, const LodSection *Source) {
  *Out = *Source;
  Out->Storage = NULL;
  if (Source->Storage == NULL) {
    return true;
  }
  Out->Storage = malloc(Source->StorageBytes);
  if (Out->Storage == NULL) {
    LodSectionInit(Out);
    return false;
  }
  memcpy(Out->Storage, Source->Storage, Source->StorageBytes);
  return true;
}

bool LodSectionCompactDense(LodSection *Section, LodEncodeScratch *Build,
                            LodVoxel **OutReleased) {
  *OutReleased = NULL;
  LodVoxel *Voxels = LodSectionDenseVoxels(Section);
  if (Voxels == NULL) {
    return true;
  }
  if (AllEqual(Voxels)) {
    Section->Mode = LOD_STORAGE_UNIFORM;
    Section->Uniform = Voxels[0];
    Section->PaletteCount = 0;
    Section->Storage = NULL;
    Section->StorageBytes = 0;
    *OutReleased = Voxels;
    return true;
  }
  BuildPalette(Voxels, Build);
  if (Build->Count > LOD_PACKED8_MAX_PALETTE) {
    // Genuinely dense: the buffer it already has is its storage.
    return true;
  }
  bool FourBit = Build->Count <= LOD_PACKED4_MAX_PALETTE;
  size_t Bytes = 0;
  void *Storage = EncodePacked(Build, FourBit, &Bytes);
  if (Storage == NULL) {
    return false;
  }
  Section->Mode = FourBit ? LOD_STORAGE_PACKED4 : LOD_STORAGE_PACKED8;
  Section->Uniform = LOD_VOXEL_AIR;
  Section->PaletteCount = Build->Count;
  Section->Storage = Storage;
  Section->StorageBytes = Bytes;
  *OutReleased = Voxels;
  return true;
}

bool LodSectionEncode(LodSection *Section,
                      const LodVoxel Voxels[LOD_SECTION_VOLUME]) {
  LodEncodeScratch *Scratch = LodEncodeScratchCreate();
  if (Scratch == NULL) {
    return false;
  }
  bool Encoded = LodSectionEncodeWithScratch(Section, Voxels, Scratch);
  LodEncodeScratchFree(Scratch);
  return Encoded;
}

size_t LodSectionHeapBytes(const LodSection *Section) {
  return Section->StorageBytes;
}

uint64_t LodSectionContentHash(const LodSection *Section, uint64_t Seed) {
  uint64_t Hash = Seed;
  for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
    LodVoxel Value = GetByIndex(Section, Idx);
    for (unsigned Byte = 0; Byte < sizeof(LodVoxel); Byte++) {
      Hash ^= (uint64_t)((Value >> (Byte * BYTE_BITS)) & (unsigned)BYTE_MASK);
      Hash *= FNV_PRIME;
    }
  }
  return Hash;
}
