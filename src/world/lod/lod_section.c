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

typedef struct {
  uint16_t Slots[PALETTE_TABLE_SIZE];
  LodVoxel Palette[LOD_SECTION_VOLUME];
  uint16_t Indices[LOD_SECTION_VOLUME];
  int Count;
} PaletteBuild;

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
  for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
    Out[Idx] = GetByIndex(Section, Idx);
  }
}

// Palette in order of first occurrence, so equal content always encodes to
// equal bytes.
static void BuildPalette(const LodVoxel *Voxels, PaletteBuild *Build) {
  // Every byte 0xFF makes every slot PALETTE_EMPTY.
  memset(Build->Slots, BYTE_MASK, sizeof(Build->Slots));
  Build->Count = 0;
  for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
    LodVoxel Value = Voxels[Idx];
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
  }
}

static void *EncodePacked(const PaletteBuild *Build, bool FourBit,
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

bool LodSectionEncode(LodSection *Section,
                      const LodVoxel Voxels[LOD_SECTION_VOLUME]) {
  PaletteBuild *Build = (PaletteBuild *)malloc(sizeof(PaletteBuild));
  if (Build == NULL) {
    return false;
  }
  BuildPalette(Voxels, Build);

  LodStorageMode Mode = LOD_STORAGE_DENSE;
  void *Storage = NULL;
  size_t Bytes = 0;
  int PaletteCount = 0;
  if (Build->Count == 1) {
    Mode = LOD_STORAGE_UNIFORM;
  } else if (Build->Count <= LOD_PACKED4_MAX_PALETTE) {
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
  free(Build);
  if (Mode != LOD_STORAGE_UNIFORM && Storage == NULL) {
    return false;
  }

  free(Section->Storage);
  Section->Mode = Mode;
  Section->Uniform = (Mode == LOD_STORAGE_UNIFORM) ? Voxels[0] : LOD_VOXEL_AIR;
  Section->PaletteCount = PaletteCount;
  Section->Storage = Storage;
  Section->StorageBytes = Bytes;
  return true;
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
