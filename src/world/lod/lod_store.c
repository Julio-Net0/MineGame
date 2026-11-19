#include "world/lod/lod_store.h"

#include <stdlib.h>
#include <string.h>

enum {
  LOD_SLOT_EMPTY = 0,
  LOD_SLOT_TOMBSTONE = -1,
  INITIAL_CAPACITY = 1024,
  DEFAULT_RADIUS = 32,
  // Rehash once used plus tombstone slots pass three quarters of the table.
  LOAD_NUMERATOR = 3,
  LOAD_DENOMINATOR = 4,
  OCTANT_CELLS = 8,
  CELL_SPAN_IN_OCTANT = 2
};

#define HASH_MUL_X 0x9E3779B97F4A7C15ULL
#define HASH_MUL_Y 0xC2B2AE3D27D4EB4FULL
#define HASH_MUL_Z 0x165667B19E3779F9ULL
#define HASH_MUL_LEVEL 0x27D4EB2F165667C5ULL
#define HASH_FINAL_SHIFT 29U
#define LEVEL_HASH_SEED 0xCBF29CE484222325ULL
#define HALF_WORD_SHIFT 32U

void LodStoreDefaultConfig(LodStoreConfig *Config) {
  for (int Level = 0; Level <= LOD_MAX_LEVEL; Level++) {
    Config->Radius[Level] = DEFAULT_RADIUS;
    Config->VerticalRadius[Level] = 0;
  }
}

static uint64_t HashKey(int Level, int X, int Y, int Z) {
  uint64_t Hash = ((uint64_t)(uint32_t)X * HASH_MUL_X) ^
                  ((uint64_t)(uint32_t)Y * HASH_MUL_Y) ^
                  ((uint64_t)(uint32_t)Z * HASH_MUL_Z) ^
                  ((uint64_t)(uint32_t)Level * HASH_MUL_LEVEL);
  return Hash ^ (Hash >> HASH_FINAL_SHIFT);
}

static LodStoreSlot *AllocSlots(int Capacity) {
  return (LodStoreSlot *)calloc((size_t)Capacity, sizeof(LodStoreSlot));
}

bool LodStoreInit(LodStore *Store, const LodStoreConfig *Config) {
  *Store = (LodStore){0};
  Store->Config = *Config;
  Store->Slots = AllocSlots(INITIAL_CAPACITY);
  if (Store->Slots == NULL) {
    return false;
  }
  Store->Capacity = INITIAL_CAPACITY;
  return true;
}

static void FreeSlotSection(LodStoreSlot *Slot) {
  LodSectionFree(Slot->Section);
  free(Slot->Section);
  Slot->Section = NULL;
}

void LodStoreFree(LodStore *Store) {
  if (Store->Slots != NULL) {
    for (int Idx = 0; Idx < Store->Capacity; Idx++) {
      if (Store->Slots[Idx].Level > 0) {
        FreeSlotSection(&Store->Slots[Idx]);
      }
    }
  }
  free(Store->Slots);
  *Store = (LodStore){0};
}

static int FindSlot(const LodStore *Store, int Level, int X, int Y, int Z) {
  unsigned Mask = (unsigned)Store->Capacity - 1U;
  unsigned Idx = (unsigned)HashKey(Level, X, Y, Z) & Mask;
  for (int Probe = 0; Probe < Store->Capacity; Probe++) {
    const LodStoreSlot *Slot = &Store->Slots[Idx];
    if (Slot->Level == LOD_SLOT_EMPTY) {
      return -1;
    }
    if (Slot->Level == Level && Slot->X == X && Slot->Y == Y && Slot->Z == Z) {
      return (int)Idx;
    }
    Idx = (Idx + 1U) & Mask;
  }
  return -1;
}

// Inserts a section pointer into a table known to have no slot for its key.
static void PlaceSlot(LodStoreSlot *Slots, int Capacity, LodStoreSlot Entry) {
  unsigned Mask = (unsigned)Capacity - 1U;
  unsigned Idx = (unsigned)HashKey(Entry.Level, Entry.X, Entry.Y, Entry.Z) & Mask;
  while (Slots[Idx].Level > 0) {
    Idx = (Idx + 1U) & Mask;
  }
  Slots[Idx] = Entry;
}

static bool Rehash(LodStore *Store, int Capacity) {
  LodStoreSlot *Slots = AllocSlots(Capacity);
  if (Slots == NULL) {
    return false;
  }
  for (int Idx = 0; Idx < Store->Capacity; Idx++) {
    if (Store->Slots[Idx].Level > 0) {
      PlaceSlot(Slots, Capacity, Store->Slots[Idx]);
    }
  }
  free(Store->Slots);
  Store->Slots = Slots;
  Store->Capacity = Capacity;
  Store->Tombstones = 0;
  return true;
}

static bool ReserveOne(LodStore *Store) {
  int Used = Store->Count + Store->Tombstones + 1;
  if (Used * LOAD_DENOMINATOR <= Store->Capacity * LOAD_NUMERATOR) {
    return true;
  }
  // Mostly tombstones: rebuilding at the same size is enough.
  bool Grow = (Store->Count + 1) * 2 > Store->Capacity;
  return Rehash(Store, Grow ? Store->Capacity * 2 : Store->Capacity);
}

static LodSection *GetOrCreate(LodStore *Store, int Level, int X, int Y, int Z) {
  int Found = FindSlot(Store, Level, X, Y, Z);
  if (Found >= 0) {
    return Store->Slots[Found].Section;
  }
  if (!ReserveOne(Store)) {
    return NULL;
  }
  LodSection *Section = (LodSection *)malloc(sizeof(LodSection));
  if (Section == NULL) {
    return NULL;
  }
  LodSectionInit(Section);

  unsigned Mask = (unsigned)Store->Capacity - 1U;
  unsigned Idx = (unsigned)HashKey(Level, X, Y, Z) & Mask;
  while (Store->Slots[Idx].Level > 0) {
    Idx = (Idx + 1U) & Mask;
  }
  if (Store->Slots[Idx].Level == LOD_SLOT_TOMBSTONE) {
    Store->Tombstones--;
  }
  Store->Slots[Idx] = (LodStoreSlot){
      .X = X, .Y = Y, .Z = Z, .Level = Level, .Section = Section};
  Store->Count++;
  return Section;
}

const LodSection *LodStoreFind(const LodStore *Store, int Level, int X, int Y,
                               int Z) {
  int Found = FindSlot(Store, Level, X, Y, Z);
  return (Found >= 0) ? Store->Slots[Found].Section : NULL;
}

bool LodStoreInRange(const LodStore *Store, int Level, int X, int Y, int Z) {
  if (Level < 1 || Level > LOD_MAX_LEVEL) {
    return false;
  }
  if (!Store->HasAnchor) {
    return true;
  }
  int64_t Dx = (int64_t)X - LodFloorShift(Store->AnchorChunkX, Level);
  int64_t Dz = (int64_t)Z - LodFloorShift(Store->AnchorChunkZ, Level);
  int64_t Radius = Store->Config.Radius[Level];
  if ((Dx * Dx) + (Dz * Dz) > Radius * Radius) {
    return false;
  }
  int Vertical = Store->Config.VerticalRadius[Level];
  if (Vertical > 0) {
    int64_t Dy = (int64_t)Y - LodFloorShift(Store->AnchorChunkY, Level);
    if (Dy > Vertical || Dy < -(int64_t)Vertical) {
      return false;
    }
  }
  return true;
}

void LodStoreSetAnchor(LodStore *Store, int ChunkX, int ChunkY, int ChunkZ) {
  Store->HasAnchor = true;
  Store->AnchorChunkX = ChunkX;
  Store->AnchorChunkY = ChunkY;
  Store->AnchorChunkZ = ChunkZ;
  for (int Idx = 0; Idx < Store->Capacity; Idx++) {
    LodStoreSlot *Slot = &Store->Slots[Idx];
    if (Slot->Level > 0 &&
        !LodStoreInRange(Store, Slot->Level, Slot->X, Slot->Y, Slot->Z)) {
      FreeSlotSection(Slot);
      Slot->Level = LOD_SLOT_TOMBSTONE;
      Store->Count--;
      Store->Tombstones++;
      Store->Evicted++;
    }
  }
}

// The 64-bit cell mask of one octant (each octant is 2x2x2 cells).
static uint64_t OctantCellMask(int OctX, int OctY, int OctZ) {
  uint64_t Mask = 0;
  for (int Local = 0; Local < OCTANT_CELLS; Local++) {
    Mask |= LodCellBit((OctX * CELL_SPAN_IN_OCTANT) + (Local & 1),
                       (OctY * CELL_SPAN_IN_OCTANT) + ((Local >> 1) & 1),
                       (OctZ * CELL_SPAN_IN_OCTANT) + ((Local >> 2) & 1));
  }
  return Mask;
}

// One octant write: which of its 8 cells to write (bit = local cell index
// x | y << 1 | z << 2) and which of those carry real data.
typedef struct {
  int OctX;
  int OctY;
  int OctZ;
  unsigned WriteMask;
  unsigned IngestedMask;
} OctantWrite;

// Copies the patch's cells selected by the write into the voxel array and
// updates provenance. Returns whether voxels or provenance changed.
static bool ApplyOctant(LodSection *Section, LodVoxel *Voxels,
                        const LodPatch8 *Patch, const OctantWrite *Write) {
  bool Changed = false;
  for (int Local = 0; Local < OCTANT_CELLS; Local++) {
    if ((Write->WriteMask & (1U << (unsigned)Local)) == 0U) {
      continue;
    }
    int CellX = (Local & 1);
    int CellY = (Local >> 1) & 1;
    int CellZ = (Local >> 2) & 1;
    for (int X = 0; X < LOD_CELL_SIZE; X++) {
      for (int Y = 0; Y < LOD_CELL_SIZE; Y++) {
        for (int Z = 0; Z < LOD_CELL_SIZE; Z++) {
          int PatchX = (CellX * LOD_CELL_SIZE) + X;
          int PatchY = (CellY * LOD_CELL_SIZE) + Y;
          int PatchZ = (CellZ * LOD_CELL_SIZE) + Z;
          int Target = LodVoxelIndex((Write->OctX * LOD_PATCH_SIZE) + PatchX,
                                     (Write->OctY * LOD_PATCH_SIZE) + PatchY,
                                     (Write->OctZ * LOD_PATCH_SIZE) + PatchZ);
          LodVoxel Value = Patch->Voxels[LodPatchIndex(PatchX, PatchY, PatchZ)];
          if (Voxels[Target] != Value) {
            Voxels[Target] = Value;
            Changed = true;
          }
        }
      }
    }
    uint64_t Bit = LodCellBit((Write->OctX * CELL_SPAN_IN_OCTANT) + CellX,
                              (Write->OctY * CELL_SPAN_IN_OCTANT) + CellY,
                              (Write->OctZ * CELL_SPAN_IN_OCTANT) + CellZ);
    bool Ingested = (Write->IngestedMask & (1U << (unsigned)Local)) != 0U;
    uint64_t Present = Section->PresentCells | Bit;
    uint64_t Real = Ingested ? (Section->IngestedCells | Bit)
                             : (Section->IngestedCells & ~Bit);
    if (Present != Section->PresentCells || Real != Section->IngestedCells) {
      Changed = true;
    }
    Section->PresentCells = Present;
    Section->IngestedCells = Real;
  }
  return Changed;
}

// Cells of the octant already holding real data refuse synthetic writes.
static unsigned ProtectedCells(const LodSection *Section, int OctX, int OctY,
                               int OctZ) {
  unsigned Protected = 0;
  for (int Local = 0; Local < OCTANT_CELLS; Local++) {
    uint64_t Bit = LodCellBit((OctX * CELL_SPAN_IN_OCTANT) + (Local & 1),
                              (OctY * CELL_SPAN_IN_OCTANT) + ((Local >> 1) & 1),
                              (OctZ * CELL_SPAN_IN_OCTANT) + ((Local >> 2) & 1));
    if ((Section->IngestedCells & Bit) != 0U) {
      Protected |= 1U << (unsigned)Local;
    }
  }
  return Protected;
}

typedef struct {
  LodVoxel Voxels[LOD_SECTION_VOLUME];
  LodPatch8 Patch;
} WriteScratch;

// One level of a write. On success *OutChanged says whether the section
// changed, and Scratch->Patch holds the section's downsampled patch for the
// level above.
static LodWriteResult WriteLevel(LodStore *Store, int Level, int ChildX,
                                 int ChildY, int ChildZ, OctantWrite Write,
                                 WriteScratch *Scratch, bool *OutChanged) {
  *OutChanged = false;
  int X = LodFloorShift(ChildX, 1);
  int Y = LodFloorShift(ChildY, 1);
  int Z = LodFloorShift(ChildZ, 1);
  if (!LodStoreInRange(Store, Level, X, Y, Z)) {
    return LOD_WRITE_OUT_OF_RANGE;
  }
  Write.OctX = ChildX - (X * 2);
  Write.OctY = ChildY - (Y * 2);
  Write.OctZ = ChildZ - (Z * 2);

  // A cell arriving without real data never replaces one that has it.
  const LodSection *Existing = LodStoreFind(Store, Level, X, Y, Z);
  if (Existing != NULL) {
    unsigned Protected =
        ProtectedCells(Existing, Write.OctX, Write.OctY, Write.OctZ);
    Write.WriteMask &= ~(Protected & ~Write.IngestedMask);
  }
  if (Write.WriteMask == 0U) {
    return LOD_WRITE_SKIPPED_PRECEDENCE;
  }

  LodSection *Section = GetOrCreate(Store, Level, X, Y, Z);
  if (Section == NULL) {
    return LOD_WRITE_NO_MEMORY;
  }
  LodSectionDecode(Section, Scratch->Voxels);
  uint64_t OldPresent = Section->PresentCells;
  uint64_t OldIngested = Section->IngestedCells;
  if (!ApplyOctant(Section, Scratch->Voxels, &Scratch->Patch, &Write)) {
    return LOD_WRITE_UNCHANGED;
  }
  if (!LodSectionEncode(Section, Scratch->Voxels)) {
    Section->PresentCells = OldPresent;
    Section->IngestedCells = OldIngested;
    return LOD_WRITE_NO_MEMORY;
  }
  Section->Revision++;
  Section->Dirty = true;
  *OutChanged = true;
  LodDownsampleVoxels(Scratch->Voxels, &Scratch->Patch);
  return LOD_WRITE_APPLIED;
}

// The octant write a changed level-L section hands its parent: one parent cell
// per child octant that holds data, real where the octant holds any real data.
static OctantWrite ParentWrite(const LodSection *Section) {
  OctantWrite Write = {0};
  for (int Local = 0; Local < OCTANT_CELLS; Local++) {
    uint64_t Mask =
        OctantCellMask(Local & 1, (Local >> 1) & 1, (Local >> 2) & 1);
    if ((Section->PresentCells & Mask) != 0U) {
      Write.WriteMask |= 1U << (unsigned)Local;
    }
    if ((Section->IngestedCells & Mask) != 0U) {
      Write.IngestedMask |= 1U << (unsigned)Local;
    }
  }
  return Write;
}

LodWriteResult LodStoreWritePatch(LodStore *Store, int Level, int ChildX,
                                  int ChildY, int ChildZ,
                                  const LodPatch8 *Patch, LodOrigin Origin) {
  if (Level < 1 || Level > LOD_MAX_LEVEL) {
    return LOD_WRITE_OUT_OF_RANGE;
  }
  WriteScratch *Scratch = (WriteScratch *)malloc(sizeof(WriteScratch));
  if (Scratch == NULL) {
    return LOD_WRITE_NO_MEMORY;
  }
  Scratch->Patch = *Patch;

  bool Ingested = Origin == LOD_ORIGIN_INGESTED;
  OctantWrite Write = {.WriteMask = (1U << OCTANT_CELLS) - 1U,
                       .IngestedMask = Ingested ? (1U << OCTANT_CELLS) - 1U : 0U};
  bool Changed = false;
  LodWriteResult Result = WriteLevel(Store, Level, ChildX, ChildY, ChildZ, Write,
                                     Scratch, &Changed);

  int CellX = ChildX;
  int CellY = ChildY;
  int CellZ = ChildZ;
  for (int Upper = Level + 1; Changed && Upper <= LOD_MAX_LEVEL; Upper++) {
    CellX = LodFloorShift(CellX, 1);
    CellY = LodFloorShift(CellY, 1);
    CellZ = LodFloorShift(CellZ, 1);
    const LodSection *Child = LodStoreFind(Store, Upper - 1, CellX, CellY, CellZ);
    if (Child == NULL) {
      break;
    }
    // A parent cell built from synthetic child cells only is synthetic, and
    // so gives way to real data already there.
    LodWriteResult UpResult = WriteLevel(Store, Upper, CellX, CellY, CellZ,
                                         ParentWrite(Child), Scratch, &Changed);
    if (UpResult == LOD_WRITE_NO_MEMORY) {
      Result = LOD_WRITE_NO_MEMORY;
    }
  }
  free(Scratch);
  return Result;
}

void LodStoreGetStats(const LodStore *Store, LodStoreStats *Out) {
  *Out = (LodStoreStats){0};
  for (int Idx = 0; Idx < Store->Capacity; Idx++) {
    const LodStoreSlot *Slot = &Store->Slots[Idx];
    if (Slot->Level <= 0) {
      continue;
    }
    Out->Sections++;
    Out->SectionsPerLevel[Slot->Level]++;
    Out->SectionsPerMode[Slot->Section->Mode]++;
    Out->SectionBytes +=
        sizeof(LodSection) + LodSectionHeapBytes(Slot->Section);
  }
  Out->TableBytes = (size_t)Store->Capacity * sizeof(LodStoreSlot);
  Out->TotalBytes = Out->SectionBytes + Out->TableBytes;
  Out->Evicted = Store->Evicted;
}

static int CompareSlotCoords(const void *A, const void *B) {
  const LodStoreSlot *SlotA = *(const LodStoreSlot *const *)A;
  const LodStoreSlot *SlotB = *(const LodStoreSlot *const *)B;
  if (SlotA->X != SlotB->X) {
    return (SlotA->X < SlotB->X) ? -1 : 1;
  }
  if (SlotA->Y != SlotB->Y) {
    return (SlotA->Y < SlotB->Y) ? -1 : 1;
  }
  if (SlotA->Z != SlotB->Z) {
    return (SlotA->Z < SlotB->Z) ? -1 : 1;
  }
  return 0;
}

static uint64_t MixWord(uint64_t Hash, uint32_t Value) {
  return (Hash ^ (uint64_t)Value) * HASH_MUL_X;
}

static uint64_t MixCoord(uint64_t Hash, int32_t Value) {
  return MixWord(Hash, (uint32_t)Value);
}

static uint64_t MixCells(uint64_t Hash, uint64_t Cells) {
  Hash = MixWord(Hash, (uint32_t)Cells);
  return MixWord(Hash, (uint32_t)(Cells >> HALF_WORD_SHIFT));
}

uint64_t LodStoreLevelHash(const LodStore *Store, int Level) {
  int Count = 0;
  for (int Idx = 0; Idx < Store->Capacity; Idx++) {
    if (Store->Slots[Idx].Level == Level) {
      Count++;
    }
  }
  if (Count == 0) {
    return 0;
  }
  const LodStoreSlot **Sorted =
      (const LodStoreSlot **)malloc((size_t)Count * sizeof(LodStoreSlot *));
  if (Sorted == NULL) {
    return 0;
  }
  int Fill = 0;
  for (int Idx = 0; Idx < Store->Capacity; Idx++) {
    if (Store->Slots[Idx].Level == Level) {
      Sorted[Fill++] = &Store->Slots[Idx];
    }
  }
  qsort((void *)Sorted, (size_t)Count, sizeof(LodStoreSlot *),
        CompareSlotCoords);

  uint64_t Hash = LEVEL_HASH_SEED;
  for (int Idx = 0; Idx < Count; Idx++) {
    const LodStoreSlot *Slot = Sorted[Idx];
    Hash = MixCoord(Hash, Slot->X);
    Hash = MixCoord(Hash, Slot->Y);
    Hash = MixCoord(Hash, Slot->Z);
    Hash = MixCells(Hash, Slot->Section->PresentCells);
    Hash = MixCells(Hash, Slot->Section->IngestedCells);
    Hash = LodSectionContentHash(Slot->Section, Hash);
  }
  free((void *)Sorted);
  return Hash;
}
