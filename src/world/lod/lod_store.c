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
  PENDING_INITIAL = 256,
  DEFAULT_HOT_LIMIT = 512,
  // Spare dense buffers kept (1 MB); more are freed.
  DENSE_POOL_MAX = 64,
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
  Config->HotLimit = DEFAULT_HOT_LIMIT;
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

// Workspace for one write and its propagation, owned by the store so writes
// allocate nothing but section storage.
struct LodStoreScratch {
  LodVoxel Voxels[LOD_SECTION_VOLUME];
  LodPatch8 Patch;
};

bool LodStoreInit(LodStore *Store, const LodStoreConfig *Config) {
  *Store = (LodStore){0};
  Store->Config = *Config;
  Store->Slots = AllocSlots(INITIAL_CAPACITY);
  Store->Scratch = (LodStoreScratch *)malloc(sizeof(LodStoreScratch));
  Store->Encode = LodEncodeScratchCreate();
  Store->DensePool = (LodVoxel **)calloc(DENSE_POOL_MAX, sizeof(LodVoxel *));
  Store->Journal = (LodStoreKey *)malloc(sizeof(LodStoreKey) * LOD_JOURNAL_SIZE);
  if (Store->Slots == NULL || Store->Scratch == NULL || Store->Encode == NULL ||
      Store->DensePool == NULL || Store->Journal == NULL) {
    LodStoreFree(Store);
    return false;
  }
  Store->Capacity = INITIAL_CAPACITY;
  return true;
}

static void FreeSlotSection(LodStore *Store, LodStoreSlot *Slot) {
  Store->HeapBytes -= LodSectionHeapBytes(Slot->Section);
  if (Slot->Section->Hot) {
    Store->HotLive--;
  }
  LodSectionFree(Slot->Section);
  free(Slot->Section);
  Slot->Section = NULL;
}

void LodStoreFree(LodStore *Store) {
  if (Store->Slots != NULL) {
    for (int Idx = 0; Idx < Store->Capacity; Idx++) {
      if (Store->Slots[Idx].Level > 0) {
        FreeSlotSection(Store, &Store->Slots[Idx]);
      }
    }
  }
  if (Store->DensePool != NULL) {
    for (int Idx = 0; Idx < Store->DensePoolCount; Idx++) {
      free(Store->DensePool[Idx]);
    }
  }
  free((void *)Store->DensePool);
  free(Store->Slots);
  free(Store->Scratch);
  free(Store->Pending);
  free(Store->Hot);
  free(Store->Journal);
  LodEncodeScratchFree(Store->Encode);
  *Store = (LodStore){0};
}

size_t LodStoreScratchBytes(void) {
  return sizeof(LodStoreScratch) + LodEncodeScratchBytes();
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

static void JournalChange(LodStore *Store, LodStoreKey Key) {
  Store->Journal[Store->JournalSeq & (LOD_JOURNAL_SIZE - 1U)] = Key;
  Store->JournalSeq++;
}

static void EvictOutOfRange(LodStore *Store) {
  for (int Idx = 0; Idx < Store->Capacity; Idx++) {
    LodStoreSlot *Slot = &Store->Slots[Idx];
    if (Slot->Level > 0 &&
        !LodStoreInRange(Store, Slot->Level, Slot->X, Slot->Y, Slot->Z)) {
      JournalChange(Store, (LodStoreKey){.X = Slot->X, .Y = Slot->Y,
                                         .Z = Slot->Z, .Level = Slot->Level});
      FreeSlotSection(Store, Slot);
      Slot->Level = LOD_SLOT_TOMBSTONE;
      Store->Count--;
      Store->Tombstones++;
      Store->Evicted++;
    }
  }
}

void LodStoreSetAnchor(LodStore *Store, int ChunkX, int ChunkY, int ChunkZ) {
  Store->HasAnchor = true;
  Store->AnchorChunkX = ChunkX;
  Store->AnchorChunkY = ChunkY;
  Store->AnchorChunkZ = ChunkZ;
  EvictOutOfRange(Store);
}

void LodStoreSetConfig(LodStore *Store, const LodStoreConfig *Config) {
  Store->Config = *Config;
  if (Store->HasAnchor) {
    EvictOutOfRange(Store);
  }
}

size_t LodStoreBytes(const LodStore *Store) {
  size_t Sections =
      ((size_t)Store->Count * sizeof(LodSection)) + Store->HeapBytes;
  size_t Table = (size_t)Store->Capacity * sizeof(LodStoreSlot);
  size_t Scratch = (Store->Scratch != NULL) ? LodStoreScratchBytes() : 0U;
  size_t Pool = (size_t)Store->DensePoolCount * (size_t)LOD_DENSE_BYTES;
  return Sections + Table + Scratch + Pool;
}

static LodVoxel *AcquireDense(LodStore *Store) {
  if (Store->DensePoolCount > 0) {
    return Store->DensePool[--Store->DensePoolCount];
  }
  return (LodVoxel *)malloc((size_t)LOD_DENSE_BYTES);
}

static void ReleaseDense(LodStore *Store, LodVoxel *Buffer) {
  if (Store->DensePoolCount < DENSE_POOL_MAX) {
    Store->DensePool[Store->DensePoolCount++] = Buffer;
  } else {
    free(Buffer);
  }
}

static bool AppendKey(LodStoreKey **List, int *Count, int *Capacity,
                      LodStoreKey Key) {
  if (*Count == *Capacity) {
    int Grown = (*Capacity > 0) ? *Capacity * 2 : PENDING_INITIAL;
    LodStoreKey *Resized =
        (LodStoreKey *)realloc(*List, (size_t)Grown * sizeof(LodStoreKey));
    if (Resized == NULL) {
      return false;
    }
    *List = Resized;
    *Capacity = Grown;
  }
  (*List)[(*Count)++] = Key;
  return true;
}

// Compacts the hot section at the front of the list, if that entry is still
// live. False only when compaction ran out of memory; the entry then stays.
static bool CompactFront(LodStore *Store) {
  LodStoreKey Key = Store->Hot[Store->HotHead];
  int Found = FindSlot(Store, Key.Level, Key.X, Key.Y, Key.Z);
  LodSection *Section = (Found >= 0) ? Store->Slots[Found].Section : NULL;
  if (Section != NULL && Section->Hot) {
    size_t OldBytes = LodSectionHeapBytes(Section);
    LodVoxel *Released = NULL;
    if (!LodSectionCompactDense(Section, Store->Encode, &Released)) {
      return false;
    }
    Store->HeapBytes = Store->HeapBytes - OldBytes + LodSectionHeapBytes(Section);
    if (Released != NULL) {
      ReleaseDense(Store, Released);
    }
    Section->Hot = false;
    Store->HotLive--;
  }
  Store->HotHead++;
  // Slide the live tail down once the consumed head is the larger part.
  if (Store->HotHead == Store->HotCount) {
    Store->HotHead = 0;
    Store->HotCount = 0;
  } else if (Store->HotHead * 2 > Store->HotCount) {
    int Remaining = Store->HotCount - Store->HotHead;
    memmove(Store->Hot, Store->Hot + Store->HotHead,
            (size_t)Remaining * sizeof(LodStoreKey));
    Store->HotHead = 0;
    Store->HotCount = Remaining;
  }
  return true;
}

bool LodStoreCompactSome(LodStore *Store, int MaxEntries) {
  for (int Done = 0; Done < MaxEntries && Store->HotHead < Store->HotCount;
       Done++) {
    if (!CompactFront(Store)) {
      return false;
    }
  }
  return true;
}

void LodStoreTrimPool(LodStore *Store, int Keep) {
  while (Store->DensePoolCount > Keep && Store->DensePoolCount > 0) {
    free(Store->DensePool[--Store->DensePoolCount]);
  }
}

bool LodStoreCompactAll(LodStore *Store) {
  return LodStoreCompactSome(Store, Store->HotCount - Store->HotHead);
}

// Registers a section that just turned (or stayed) DENSE under writes, then
// keeps the hot set within its limit by compacting the oldest.
static bool MarkHot(LodStore *Store, LodSection *Section, LodStoreKey Key) {
  if (!Section->Hot) {
    if (!AppendKey(&Store->Hot, &Store->HotCount, &Store->HotCapacity, Key)) {
      return false;
    }
    Section->Hot = true;
    Store->HotLive++;
  }
  while (Store->HotLive > Store->Config.HotLimit &&
         Store->HotHead < Store->HotCount) {
    if (!CompactFront(Store)) {
      return false;
    }
  }
  return true;
}

// Gives a section whose content just changed in Voxels (scratch) DENSE
// storage holding that content, from the pool when possible.
static bool PromoteDense(LodStore *Store, LodSection *Section,
                         const LodVoxel *Voxels) {
  LodVoxel *Buffer = AcquireDense(Store);
  if (Buffer == NULL) {
    return false;
  }
  memcpy(Buffer, Voxels, (size_t)LOD_DENSE_BYTES);
  size_t OldBytes = LodSectionHeapBytes(Section);
  LodSectionAdoptDense(Section, Buffer);
  Store->HeapBytes = Store->HeapBytes - OldBytes + LodSectionHeapBytes(Section);
  return true;
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

// Marks the written cells present, and real or not as the write says.
// Returns whether either mask changed.
static bool UpdateProvenance(LodSection *Section, const OctantWrite *Write) {
  bool Changed = false;
  for (int Local = 0; Local < OCTANT_CELLS; Local++) {
    if ((Write->WriteMask & (1U << (unsigned)Local)) == 0U) {
      continue;
    }
    uint64_t Bit =
        LodCellBit((Write->OctX * CELL_SPAN_IN_OCTANT) + (Local & 1),
                   (Write->OctY * CELL_SPAN_IN_OCTANT) + ((Local >> 1) & 1),
                   (Write->OctZ * CELL_SPAN_IN_OCTANT) + ((Local >> 2) & 1));
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

// Copies the patch's cells selected by the write into the voxel array.
// Returns whether any voxel changed; provenance is the caller's.
static bool ApplyCells(LodVoxel *Voxels, const LodPatch8 *Patch,
                       const OctantWrite *Write) {
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
  }
  return Changed;
}

// Whether every cell the write covers holds Value throughout in the patch.
static bool PatchCellsEqual(const LodPatch8 *Patch, unsigned WriteMask,
                            LodVoxel Value) {
  for (int Local = 0; Local < OCTANT_CELLS; Local++) {
    if ((WriteMask & (1U << (unsigned)Local)) == 0U) {
      continue;
    }
    int BaseX = (Local & 1) * LOD_CELL_SIZE;
    int BaseY = ((Local >> 1) & 1) * LOD_CELL_SIZE;
    int BaseZ = ((Local >> 2) & 1) * LOD_CELL_SIZE;
    for (int X = 0; X < LOD_CELL_SIZE; X++) {
      for (int Y = 0; Y < LOD_CELL_SIZE; Y++) {
        for (int Z = 0; Z < LOD_CELL_SIZE; Z++) {
          if (Patch->Voxels[LodPatchIndex(BaseX + X, BaseY + Y, BaseZ + Z)] !=
              Value) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

// The patch a uniform section hands its parent: the rule maps a uniform grid
// to the same value everywhere.
static void FillUniformPatch(const LodSection *Section, LodPatch8 *Patch) {
  LodVoxel Value =
      (LodClassifyBlock(LodVoxelBlock(Section->Uniform)) == LOD_CLASS_AIR)
          ? LOD_VOXEL_AIR
          : Section->Uniform;
  LodPatchFill(Patch, Value);
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

// Adds a changed section to the pending set, once. Top-level sections have
// no parent and never wait.
static bool MarkPending(LodStore *Store, LodSection *Section, LodStoreKey Key) {
  if (Key.Level >= LOD_MAX_LEVEL || Section->PendingPropagation) {
    return true;
  }
  if (Store->PendingCount == Store->PendingCapacity) {
    int Capacity =
        (Store->PendingCapacity > 0) ? Store->PendingCapacity * 2 : PENDING_INITIAL;
    LodStoreKey *Grown = (LodStoreKey *)realloc(
        Store->Pending, (size_t)Capacity * sizeof(LodStoreKey));
    if (Grown == NULL) {
      return false;
    }
    Store->Pending = Grown;
    Store->PendingCapacity = Capacity;
  }
  Store->Pending[Store->PendingCount++] = Key;
  Section->PendingPropagation = true;
  return true;
}

// One level of a write, from Scratch->Patch. A changed section joins the
// pending set so its parent is rebuilt at the next flush.
static LodWriteResult WriteLevel(LodStore *Store, int Level, int ChildX,
                                 int ChildY, int ChildZ, OctantWrite Write,
                                 LodStoreScratch *Scratch) {
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

  // A uniform section receiving its own value (air into air, rock into rock,
  // the bulk of any load) keeps its content: only provenance can change, and
  // the parent gets the same uniform patch it would get after a full rebuild.
  LodStoreKey Key = {.X = X, .Y = Y, .Z = Z, .Level = Level};
  if (Section->Mode == LOD_STORAGE_UNIFORM &&
      PatchCellsEqual(&Scratch->Patch, Write.WriteMask, Section->Uniform)) {
    if (!UpdateProvenance(Section, &Write)) {
      return LOD_WRITE_UNCHANGED;
    }
    return MarkPending(Store, Section, Key) ? LOD_WRITE_APPLIED
                                            : LOD_WRITE_NO_MEMORY;
  }

  // A section already DENSE (hot) is written in place. Any other is decoded
  // once and, if its content changes, turns hot instead of being re-encoded:
  // compaction happens once, later, however many writes follow.
  bool ContentChanged = false;
  LodVoxel *Dense = LodSectionDenseVoxels(Section);
  if (Dense != NULL) {
    ContentChanged = ApplyCells(Dense, &Scratch->Patch, &Write);
  } else {
    LodSectionDecode(Section, Scratch->Voxels);
    ContentChanged = ApplyCells(Scratch->Voxels, &Scratch->Patch, &Write);
    if (ContentChanged && !PromoteDense(Store, Section, Scratch->Voxels)) {
      return LOD_WRITE_NO_MEMORY;
    }
  }
  bool ProvenanceChanged = UpdateProvenance(Section, &Write);
  if (!ContentChanged && !ProvenanceChanged) {
    return LOD_WRITE_UNCHANGED;
  }
  if (ContentChanged) {
    Section->Revision++;
    Section->Dirty = true;
    JournalChange(Store, Key);
    if (!MarkHot(Store, Section, Key)) {
      return LOD_WRITE_NO_MEMORY;
    }
  }
  return MarkPending(Store, Section, Key) ? LOD_WRITE_APPLIED
                                          : LOD_WRITE_NO_MEMORY;
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

LodWriteResult LodStoreWritePatchDeferred(LodStore *Store, int Level,
                                          int ChildX, int ChildY, int ChildZ,
                                          const LodPatch8 *Patch,
                                          LodOrigin Origin) {
  if (Level < 1 || Level > LOD_MAX_LEVEL) {
    return LOD_WRITE_OUT_OF_RANGE;
  }
  LodStoreScratch *Scratch = Store->Scratch;
  Scratch->Patch = *Patch;
  bool Ingested = Origin == LOD_ORIGIN_INGESTED;
  OctantWrite Write = {.WriteMask = (1U << OCTANT_CELLS) - 1U,
                       .IngestedMask = Ingested ? (1U << OCTANT_CELLS) - 1U : 0U};
  return WriteLevel(Store, Level, ChildX, ChildY, ChildZ, Write, Scratch);
}

static int CompareChunkWrites(const void *A, const void *B) {
  const LodChunkWrite *WriteA = (const LodChunkWrite *)A;
  const LodChunkWrite *WriteB = (const LodChunkWrite *)B;
  int Keys[2][4] = {{LodFloorShift(WriteA->ChunkX, 1),
                     LodFloorShift(WriteA->ChunkY, 1),
                     LodFloorShift(WriteA->ChunkZ, 1), WriteA->Sequence},
                    {LodFloorShift(WriteB->ChunkX, 1),
                     LodFloorShift(WriteB->ChunkY, 1),
                     LodFloorShift(WriteB->ChunkZ, 1), WriteB->Sequence}};
  for (int Key = 0; Key < 4; Key++) {
    if (Keys[0][Key] != Keys[1][Key]) {
      return (Keys[0][Key] < Keys[1][Key]) ? -1 : 1;
    }
  }
  return 0;
}

static OctantWrite RealOctantWrite(const LodChunkWrite *Write, int X, int Y,
                                   int Z) {
  return (OctantWrite){.OctX = Write->ChunkX - (X * 2),
                       .OctY = Write->ChunkY - (Y * 2),
                       .OctZ = Write->ChunkZ - (Z * 2),
                       .WriteMask = (1U << OCTANT_CELLS) - 1U,
                       .IngestedMask = (1U << OCTANT_CELLS) - 1U};
}

// Applies Writes[First, Last), all in one level-1 section, in Sequence order.
static void WriteSectionGroup(LodStore *Store, const LodChunkWrite *Writes,
                              int First, int Last, LodBatchResult *Out) {
  int X = LodFloorShift(Writes[First].ChunkX, 1);
  int Y = LodFloorShift(Writes[First].ChunkY, 1);
  int Z = LodFloorShift(Writes[First].ChunkZ, 1);
  int Count = Last - First;
  if (!LodStoreInRange(Store, 1, X, Y, Z)) {
    Out->OutOfRange += Count;
    return;
  }
  LodSection *Section = GetOrCreate(Store, 1, X, Y, Z);
  if (Section == NULL) {
    Out->Failed += Count;
    return;
  }
  LodStoreKey Key = {.X = X, .Y = Y, .Z = Z, .Level = 1};

  // Real data replaces anything, so no cell is protected; a uniform section
  // that every write leaves as it was only gains provenance.
  bool KeepsContent = Section->Mode == LOD_STORAGE_UNIFORM;
  for (int Idx = First; Idx < Last && KeepsContent; Idx++) {
    KeepsContent = PatchCellsEqual(Writes[Idx].Patch, (1U << OCTANT_CELLS) - 1U,
                                   Section->Uniform);
  }
  bool Changed = false;
  if (KeepsContent) {
    for (int Idx = First; Idx < Last; Idx++) {
      OctantWrite Write = RealOctantWrite(&Writes[Idx], X, Y, Z);
      Changed = UpdateProvenance(Section, &Write) || Changed;
    }
  } else {
    LodVoxel *Dense = LodSectionDenseVoxels(Section);
    LodVoxel *Voxels = Dense;
    if (Voxels == NULL) {
      Voxels = Store->Scratch->Voxels;
      LodSectionDecode(Section, Voxels);
    }
    bool ContentChanged = false;
    for (int Idx = First; Idx < Last; Idx++) {
      OctantWrite Write = RealOctantWrite(&Writes[Idx], X, Y, Z);
      ContentChanged =
          ApplyCells(Voxels, Writes[Idx].Patch, &Write) || ContentChanged;
      Changed = UpdateProvenance(Section, &Write) || Changed;
    }
    if (ContentChanged) {
      if (Dense == NULL && !PromoteDense(Store, Section, Voxels)) {
        Out->Failed += Count;
        return;
      }
      Section->Revision++;
      Section->Dirty = true;
      JournalChange(Store, Key);
      Changed = true;
      if (!MarkHot(Store, Section, Key)) {
        Out->Failed += Count;
        return;
      }
    }
  }
  if (Changed && !MarkPending(Store, Section, Key)) {
    Out->Failed += Count;
    return;
  }
  Out->Applied += Count;
}

void LodStoreWriteChunksDeferred(LodStore *Store, LodChunkWrite *Writes,
                                 int Count, LodBatchResult *Out) {
  *Out = (LodBatchResult){0};
  if (Count <= 0) {
    return;
  }
  qsort((void *)Writes, (size_t)Count, sizeof(LodChunkWrite),
        CompareChunkWrites);
  int First = 0;
  for (int Idx = 1; Idx <= Count; Idx++) {
    bool SameSection =
        Idx < Count &&
        LodFloorShift(Writes[Idx].ChunkX, 1) ==
            LodFloorShift(Writes[First].ChunkX, 1) &&
        LodFloorShift(Writes[Idx].ChunkY, 1) ==
            LodFloorShift(Writes[First].ChunkY, 1) &&
        LodFloorShift(Writes[Idx].ChunkZ, 1) ==
            LodFloorShift(Writes[First].ChunkZ, 1);
    if (!SameSection) {
      WriteSectionGroup(Store, Writes, First, Idx, Out);
      First = Idx;
    }
  }
}

// Entries are taken in order while the list grows: a parent changed by its
// children is appended behind them, so draining the list reaches the top. A
// section changed again after it was processed is appended again.
bool LodStoreFlushSome(LodStore *Store, int MaxEntries) {
  LodStoreScratch *Scratch = Store->Scratch;
  bool Passed = true;
  int Processed = 0;
  for (; Processed < MaxEntries && Processed < Store->PendingCount;
       Processed++) {
    LodStoreKey Key = Store->Pending[Processed];
    int Found = FindSlot(Store, Key.Level, Key.X, Key.Y, Key.Z);
    if (Found < 0) {
      continue;
    }
    LodSection *Child = Store->Slots[Found].Section;
    if (!Child->PendingPropagation) {
      continue;
    }
    Child->PendingPropagation = false;
    const LodVoxel *Dense = LodSectionDenseVoxels(Child);
    if (Child->Mode == LOD_STORAGE_UNIFORM) {
      FillUniformPatch(Child, &Scratch->Patch);
    } else if (Dense != NULL) {
      LodDownsampleVoxels(Dense, &Scratch->Patch);
    } else {
      LodSectionDecode(Child, Scratch->Voxels);
      LodDownsampleVoxels(Scratch->Voxels, &Scratch->Patch);
    }
    // A parent cell built from synthetic child cells only is synthetic, and
    // so gives way to real data already there.
    if (WriteLevel(Store, Key.Level + 1, Key.X, Key.Y, Key.Z, ParentWrite(Child),
                   Scratch) == LOD_WRITE_NO_MEMORY) {
      Passed = false;
    }
  }
  int Remaining = Store->PendingCount - Processed;
  if (Remaining > 0) {
    memmove(Store->Pending, Store->Pending + Processed,
            (size_t)Remaining * sizeof(LodStoreKey));
  }
  Store->PendingCount = Remaining;
  return Passed;
}

bool LodStoreFlush(LodStore *Store) {
  bool Passed = true;
  while (Store->PendingCount > 0) {
    Passed = LodStoreFlushSome(Store, Store->PendingCount) && Passed;
  }
  return LodStoreCompactAll(Store) && Passed;
}

LodWriteResult LodStoreWritePatch(LodStore *Store, int Level, int ChildX,
                                  int ChildY, int ChildZ,
                                  const LodPatch8 *Patch, LodOrigin Origin) {
  LodWriteResult Result = LodStoreWritePatchDeferred(Store, Level, ChildX, ChildY,
                                                     ChildZ, Patch, Origin);
  if (!LodStoreFlush(Store)) {
    Result = LOD_WRITE_NO_MEMORY;
  }
  return Result;
}

// Copies the incoming voxels of every cell real data does not hold.
static void MergeUnprotected(LodVoxel *Target, const LodSection *Incoming,
                             uint64_t Protected) {
  for (int X = 0; X < LOD_SECTION_SIZE; X++) {
    for (int Y = 0; Y < LOD_SECTION_SIZE; Y++) {
      for (int Z = 0; Z < LOD_SECTION_SIZE; Z++) {
        uint64_t Bit = LodCellBit(X / LOD_CELL_SIZE, Y / LOD_CELL_SIZE,
                                  Z / LOD_CELL_SIZE);
        if ((Protected & Bit) == 0U) {
          Target[LodVoxelIndex(X, Y, Z)] = LodSectionGet(Incoming, X, Y, Z);
        }
      }
    }
  }
}

LodWriteResult LodStoreWriteSynthetic(LodStore *Store, int Level, int X, int Y,
                                      int Z, LodSection *Encoded,
                                      uint64_t PresentCells) {
  if (!LodStoreInRange(Store, Level, X, Y, Z)) {
    return LOD_WRITE_OUT_OF_RANGE;
  }
  const LodSection *Existing = LodStoreFind(Store, Level, X, Y, Z);
  uint64_t Protected = (Existing != NULL) ? Existing->IngestedCells : 0U;
  if (Protected == ~(uint64_t)0U) {
    return LOD_WRITE_SKIPPED_PRECEDENCE;
  }
  LodSection *Section = GetOrCreate(Store, Level, X, Y, Z);
  if (Section == NULL) {
    return LOD_WRITE_NO_MEMORY;
  }
  size_t OldBytes = LodSectionHeapBytes(Section);
  LodVoxel *Dense = LodSectionDenseVoxels(Section);
  if (Protected != 0U || Dense != NULL) {
    // A section holding real cells, or hot under real writes, is merged in
    // place and keeps its storage path; real cells stay as they are.
    LodVoxel *Target = (Dense != NULL) ? Dense : Store->Scratch->Voxels;
    if (Dense == NULL) {
      LodSectionDecode(Section, Target);
    }
    MergeUnprotected(Target, Encoded, Protected);
    if (Dense == NULL &&
        !LodSectionEncodeWithScratch(Section, Target, Store->Encode)) {
      return LOD_WRITE_NO_MEMORY;
    }
  } else {
    LodSectionAdoptContent(Section, Encoded);
  }
  Store->HeapBytes = Store->HeapBytes - OldBytes + LodSectionHeapBytes(Section);
  Section->PresentCells =
      (Section->PresentCells & Protected) | (PresentCells & ~Protected);
  Section->Revision++;
  Section->Dirty = true;
  JournalChange(Store, (LodStoreKey){.X = X, .Y = Y, .Z = Z, .Level = Level});
  return LOD_WRITE_APPLIED;
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
    if (Slot->Section->IngestedCells != 0U) {
      Out->SectionsIngested++;
    } else if (Slot->Section->PresentCells != 0U) {
      Out->SectionsSynthetic++;
    }
    Out->SectionBytes +=
        sizeof(LodSection) + LodSectionHeapBytes(Slot->Section);
  }
  Out->TableBytes = (size_t)Store->Capacity * sizeof(LodStoreSlot);
  Out->ScratchBytes = (Store->Scratch != NULL) ? LodStoreScratchBytes() : 0U;
  Out->PoolBytes = (size_t)Store->DensePoolCount * (size_t)LOD_DENSE_BYTES;
  Out->TotalBytes =
      Out->SectionBytes + Out->TableBytes + Out->ScratchBytes + Out->PoolBytes;
  Out->Evicted = Store->Evicted;
  Out->HotSections = Store->HotLive;
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
