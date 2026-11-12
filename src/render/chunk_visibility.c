#include "render/chunk_visibility.h"

#include "core/log.h"
#include "world/block_system.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
  CELL_Y_SHIFT = 4,
  CELL_X_SHIFT = 8,
  CELL_AXIS_MASK = CHUNK_SIZE - 1,
  CHUNK_LAST = CHUNK_SIZE - 1,
  CELL_STEP_Z = 1,
  CELL_STEP_Y = 1 << CELL_Y_SHIFT,
  CELL_STEP_X = 1 << CELL_X_SHIFT,
  WORD_SHIFT = 6,
  WORD_MASK = (1 << WORD_SHIFT) - 1,
  SNAPSHOT_MARGIN = 1,
  FACE_VIS_PAIR_BITS = 15
};

_Static_assert(CHUNK_SIZE == (1 << CELL_Y_SHIFT),
               "cell indexing packs each axis into 4 bits");

_Static_assert(FACE_VIS_WORDS << WORD_SHIFT == (int)CHUNK_VOLUME,
               "the blocked mask holds one bit per cell");

// Bit of each unordered face pair; the diagonal is unused.
static const int PAIR_INDEX[CHUNK_FACE_COUNT][CHUNK_FACE_COUNT] = {
    {-1, 0, 1, 2, 3, 4},  {0, -1, 5, 6, 7, 8},   {1, 5, -1, 9, 10, 11},
    {2, 6, 9, -1, 12, 13}, {3, 7, 10, 12, -1, 14}, {4, 8, 11, 13, 14, -1},
};

static bool *GetOpaqueTable(void) {
  static bool OpaqueById[BLOCK_REGISTRY_SIZE];
  return OpaqueById;
}

// Matches the mesher: a face is hidden only behind a neighbour that is neither
// air nor transparent, and cross geometry is forced transparent at load.
void InitChunkVisibility(void) {
  bool *OpaqueById = GetOpaqueTable();
  int OpaqueCount = 0;
  for (int Id = 0; Id < BLOCK_REGISTRY_SIZE; Id++) {
    const BlockType *Def = GetBlockDef(Id);
    bool Registered = (Id != 0) && (Def->Id == Id);
    OpaqueById[Id] = Registered && !Def->IsTransparent &&
                     Def->RenderType != BLOCK_RENDER_CROSS;
    if (OpaqueById[Id]) { OpaqueCount++; }
  }
  LogInfo("RENDER: %d block ids occlude for chunk visibility", OpaqueCount);
}

bool IsBlockOpaqueForVisibility(BlockID Id) {
  return (Id < BLOCK_REGISTRY_SIZE) && GetOpaqueTable()[Id];
}

FaceVisibility FacePairBit(ChunkFace A, ChunkFace B) {
  int Index = PAIR_INDEX[A][B];
  return (Index < 0) ? FACE_VIS_UNKNOWN : (FaceVisibility)(1U << (unsigned)Index);
}

bool FaceVisConnects(FaceVisibility Vis, ChunkFace A, ChunkFace B) {
  if ((Vis & FACE_VIS_KNOWN) == 0 || A == B) { return true; }
  return (Vis & FacePairBit(A, B)) != 0;
}

// Endpoints of each pair bit, the inverse of PAIR_INDEX.
static const uint8_t PAIR_FACE_A[FACE_VIS_PAIR_BITS] = {0, 0, 0, 0, 0, 1, 1, 1,
                                                        1, 2, 2, 2, 3, 3, 4};
static const uint8_t PAIR_FACE_B[FACE_VIS_PAIR_BITS] = {1, 2, 3, 4, 5, 2, 3, 4,
                                                        5, 3, 4, 5, 4, 5, 5};

unsigned FaceVisExits(FaceVisibility Vis, unsigned EntryMask) {
  const unsigned ALL_FACES = (1U << CHUNK_FACE_COUNT) - 1U;
  if ((Vis & FACE_VIS_KNOWN) == 0 || Vis == FACE_VIS_ALL) { return ALL_FACES; }
  unsigned Exits = EntryMask & ALL_FACES;
  for (int Pair = 0; Pair < FACE_VIS_PAIR_BITS; Pair++) {
    if ((Vis & (1U << (unsigned)Pair)) == 0) { continue; }
    unsigned A = 1U << PAIR_FACE_A[Pair];
    unsigned B = 1U << PAIR_FACE_B[Pair];
    if ((EntryMask & A) != 0) { Exits |= B; }
    if ((EntryMask & B) != 0) { Exits |= A; }
  }
  return Exits;
}

static FaceVisibility PairsWithinFaces(unsigned FaceMask) {
  FaceVisibility Pairs = 0;
  for (int A = 0; A < CHUNK_FACE_COUNT; A++) {
    if ((FaceMask & (1U << (unsigned)A)) == 0) { continue; }
    for (int B = A + 1; B < CHUNK_FACE_COUNT; B++) {
      if ((FaceMask & (1U << (unsigned)B)) != 0) {
        Pairs |= FacePairBit((ChunkFace)A, (ChunkFace)B);
      }
    }
  }
  return Pairs;
}

static unsigned FacesOfCell(int X, int Y, int Z) {
  unsigned Mask = 0;
  if (X == 0) { Mask |= 1U << CHUNK_FACE_NEG_X; }
  if (X == CHUNK_LAST) { Mask |= 1U << CHUNK_FACE_POS_X; }
  if (Y == 0) { Mask |= 1U << CHUNK_FACE_NEG_Y; }
  if (Y == CHUNK_LAST) { Mask |= 1U << CHUNK_FACE_POS_Y; }
  if (Z == 0) { Mask |= 1U << CHUNK_FACE_NEG_Z; }
  if (Z == CHUNK_LAST) { Mask |= 1U << CHUNK_FACE_POS_Z; }
  return Mask;
}

static bool IsBlocked(const FaceVisScratch *Scratch, int Cell) {
  return (Scratch->Blocked[Cell >> WORD_SHIFT] >> (unsigned)(Cell & WORD_MASK) & 1U) != 0;
}

static void SetBlocked(FaceVisScratch *Scratch, int Cell) {
  Scratch->Blocked[Cell >> WORD_SHIFT] |= (uint64_t)1U << (unsigned)(Cell & WORD_MASK);
}

// Marks and pushes a neighbour that is still open. Opaque cells start out
// blocked, so one bit answers both "solid" and "already reached".
static void PushIfOpen(FaceVisScratch *Scratch, int *Top, int Cell) {
  if (IsBlocked(Scratch, Cell)) { return; }
  SetBlocked(Scratch, Cell);
  Scratch->Stack[(*Top)++] = (uint16_t)Cell;
}

// Iterative flood fill of the open component holding Seed; returns the faces it
// touches. Each cell is pushed at most once, so the stack never exceeds the
// chunk volume.
static unsigned FloodFaces(FaceVisScratch *Scratch, int Seed) {
  int Top = 0;
  unsigned Faces = 0;
  PushIfOpen(Scratch, &Top, Seed);
  while (Top > 0) {
    int Cell = Scratch->Stack[--Top];
    int X = Cell >> CELL_X_SHIFT;
    int Y = (Cell >> CELL_Y_SHIFT) & CELL_AXIS_MASK;
    int Z = Cell & CELL_AXIS_MASK;
    Faces |= FacesOfCell(X, Y, Z);
    if (X > 0) { PushIfOpen(Scratch, &Top, Cell - CELL_STEP_X); }
    if (X < CHUNK_LAST) { PushIfOpen(Scratch, &Top, Cell + CELL_STEP_X); }
    if (Y > 0) { PushIfOpen(Scratch, &Top, Cell - CELL_STEP_Y); }
    if (Y < CHUNK_LAST) { PushIfOpen(Scratch, &Top, Cell + CELL_STEP_Y); }
    if (Z > 0) { PushIfOpen(Scratch, &Top, Cell - CELL_STEP_Z); }
    if (Z < CHUNK_LAST) { PushIfOpen(Scratch, &Top, Cell + CELL_STEP_Z); }
  }
  return Faces;
}

FaceVisibility ComputeFaceVisibility(const ChunkSnapshot *Snap,
                                     FaceVisScratch *Scratch) {
  const bool *OpaqueById = GetOpaqueTable();
  memset(Scratch->Blocked, 0, sizeof(Scratch->Blocked));

  int OpaqueCount = 0;
  for (int X = 0; X < CHUNK_SIZE; X++) {
    for (int Y = 0; Y < CHUNK_SIZE; Y++) {
      const BlockID *Row =
          Snap->Blocks[X + SNAPSHOT_MARGIN][Y + SNAPSHOT_MARGIN];
      for (int Z = 0; Z < CHUNK_SIZE; Z++) {
        BlockID Id = Row[Z + SNAPSHOT_MARGIN];
        if (Id < BLOCK_REGISTRY_SIZE && OpaqueById[Id]) {
          SetBlocked(Scratch, (X << CELL_X_SHIFT) | (Y << CELL_Y_SHIFT) | Z);
          OpaqueCount++;
        }
      }
    }
  }

  if (OpaqueCount == 0) { return FACE_VIS_ALL; }
  if (OpaqueCount == (int)CHUNK_VOLUME) { return FACE_VIS_NONE; }

  // Only components that reach the boundary can connect two faces, so seeds are
  // boundary cells: whole rows on the X/Y shell, the two Z ends elsewhere.
  FaceVisibility Pairs = 0;
  for (int X = 0; X < CHUNK_SIZE; X++) {
    for (int Y = 0; Y < CHUNK_SIZE; Y++) {
      bool Shell = (X == 0) || (X == CHUNK_LAST) || (Y == 0) || (Y == CHUNK_LAST);
      int StepZ = Shell ? 1 : CHUNK_LAST;
      for (int Z = 0; Z < CHUNK_SIZE; Z += StepZ) {
        int Cell = (X << CELL_X_SHIFT) | (Y << CELL_Y_SHIFT) | Z;
        if (IsBlocked(Scratch, Cell)) { continue; }
        Pairs |= PairsWithinFaces(FloodFaces(Scratch, Cell));
        if (Pairs == FACE_VIS_PAIRS) { return FACE_VIS_ALL; }
      }
    }
  }
  return (FaceVisibility)(Pairs | FACE_VIS_KNOWN);
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

typedef struct {
  BlockID Stone;
  BlockID Water;
  BlockID Glass;
  BlockID Flower;
} SelfTestBlocks;

static ChunkSnapshot *GetSelfTestSnapshot(void) {
  static ChunkSnapshot Snap;
  return &Snap;
}

static void FillSnapshot(ChunkSnapshot *Snap, BlockID Id) {
  memset(Snap->Blocks, 0, sizeof(Snap->Blocks));
  for (int X = 0; X < CHUNK_SIZE; X++) {
    for (int Y = 0; Y < CHUNK_SIZE; Y++) {
      for (int Z = 0; Z < CHUNK_SIZE; Z++) {
        Snap->Blocks[X + SNAPSHOT_MARGIN][Y + SNAPSHOT_MARGIN]
                    [Z + SNAPSHOT_MARGIN] = Id;
      }
    }
  }
}

static void SetCell(ChunkSnapshot *Snap, int X, int Y, int Z, BlockID Id) {
  Snap->Blocks[X + SNAPSHOT_MARGIN][Y + SNAPSHOT_MARGIN][Z + SNAPSHOT_MARGIN] =
      Id;
}

// Fills the plane where the given axis (0 = X, 1 = Y, 2 = Z) equals Coord.
static void SetPlane(ChunkSnapshot *Snap, int Axis, int Coord, BlockID Id) {
  for (int U = 0; U < CHUNK_SIZE; U++) {
    for (int V = 0; V < CHUNK_SIZE; V++) {
      if (Axis == 0) {
        SetCell(Snap, Coord, U, V, Id);
      } else if (Axis == 1) {
        SetCell(Snap, U, Coord, V, Id);
      } else {
        SetCell(Snap, U, V, Coord, Id);
      }
    }
  }
}

static FaceVisibility PairsOf(unsigned FaceMask) {
  return (FaceVisibility)(PairsWithinFaces(FaceMask) | FACE_VIS_KNOWN);
}

static bool CheckCase(const char *Name, FaceVisibility Expected,
                      FaceVisScratch *Scratch) {
  FaceVisibility Actual = ComputeFaceVisibility(GetSelfTestSnapshot(), Scratch);
  if (Actual == Expected) { return true; }
  LogError("RENDER: face visibility self-test '%s' expected 0x%04X, got 0x%04X",
           Name, (unsigned)Expected, (unsigned)Actual);
  return false;
}

static bool ResolveSelfTestBlocks(SelfTestBlocks *Out) {
  int Stone = GetBlockIdByName("Stone");
  int Water = GetBlockIdByName("Water");
  int Glass = GetBlockIdByName("Glass");
  int Flower = GetBlockIdByName("allium");
  if (Stone <= 0 || Water <= 0 || Glass <= 0 || Flower <= 0) {
    LogError("RENDER: face visibility self-test is missing Stone, Water, Glass "
             "or allium");
    return false;
  }
  *Out = (SelfTestBlocks){(BlockID)Stone, (BlockID)Water, (BlockID)Glass,
                          (BlockID)Flower};
  return true;
}

bool RunFaceVisibilitySelfTest(void) {
  SelfTestBlocks Blocks;
  if (!ResolveSelfTestBlocks(&Blocks)) { return false; }

  static FaceVisScratch Scratch;
  ChunkSnapshot *Snap = GetSelfTestSnapshot();
  const unsigned ALL_FACES = (1U << CHUNK_FACE_COUNT) - 1U;
  const int MIDDLE = CHUNK_SIZE / 2;
  int Failures = 0;
  int Cases = 0;

  FillSnapshot(Snap, 0);
  Failures += !CheckCase("air", FACE_VIS_ALL, &Scratch);
  Cases++;

  FillSnapshot(Snap, Blocks.Water);
  Failures += !CheckCase("water", FACE_VIS_ALL, &Scratch);
  Cases++;

  FillSnapshot(Snap, Blocks.Stone);
  Failures += !CheckCase("stone", FACE_VIS_NONE, &Scratch);
  Cases++;

  // A one-block tunnel along X through solid rock.
  FillSnapshot(Snap, Blocks.Stone);
  for (int X = 0; X < CHUNK_SIZE; X++) { SetCell(Snap, X, MIDDLE, MIDDLE, 0); }
  Failures += !CheckCase(
      "tunnel", PairsOf((1U << CHUNK_FACE_NEG_X) | (1U << CHUNK_FACE_POS_X)),
      &Scratch);
  Cases++;

  // Entering the tunnel from -X leads only out through +X (or back).
  unsigned TunnelExits = FaceVisExits(ComputeFaceVisibility(Snap, &Scratch),
                                      1U << CHUNK_FACE_NEG_X);
  if (TunnelExits != ((1U << CHUNK_FACE_NEG_X) | (1U << CHUNK_FACE_POS_X))) {
    LogError("RENDER: face visibility self-test 'tunnel exits' got 0x%02X",
             TunnelExits);
    Failures++;
  }
  Cases++;

  // A horizontal air slab: the four side faces see each other, top and bottom
  // stay sealed.
  FillSnapshot(Snap, Blocks.Stone);
  SetPlane(Snap, 1, MIDDLE, 0);
  Failures += !CheckCase(
      "slab", PairsOf(ALL_FACES & ~((1U << CHUNK_FACE_NEG_Y) |
                                    (1U << CHUNK_FACE_POS_Y))),
      &Scratch);
  Cases++;

  // A stone wall across X separates -X from +X; every other pair stays open.
  FillSnapshot(Snap, 0);
  SetPlane(Snap, 0, MIDDLE, Blocks.Stone);
  Failures += !CheckCase(
      "stone wall",
      (FaceVisibility)(FACE_VIS_ALL &
                       ~FacePairBit(CHUNK_FACE_NEG_X, CHUNK_FACE_POS_X)),
      &Scratch);
  Cases++;

  // The same wall in glass, then in cross flora, does not occlude.
  SetPlane(Snap, 0, MIDDLE, Blocks.Glass);
  Failures += !CheckCase("glass wall", FACE_VIS_ALL, &Scratch);
  Cases++;
  SetPlane(Snap, 0, MIDDLE, Blocks.Flower);
  Failures += !CheckCase("flora wall", FACE_VIS_ALL, &Scratch);
  Cases++;

  // One open corner cell lies on three faces at once and joins them.
  FillSnapshot(Snap, Blocks.Stone);
  SetCell(Snap, 0, 0, 0, 0);
  Failures += !CheckCase("corner",
                         PairsOf((1U << CHUNK_FACE_NEG_X) |
                                 (1U << CHUNK_FACE_NEG_Y) |
                                 (1U << CHUNK_FACE_NEG_Z)),
                         &Scratch);
  Cases++;

  // An air pocket that never reaches the boundary connects nothing.
  FillSnapshot(Snap, Blocks.Stone);
  SetCell(Snap, MIDDLE, MIDDLE, MIDDLE, 0);
  SetCell(Snap, MIDDLE + 1, MIDDLE, MIDDLE, 0);
  Failures += !CheckCase("pocket", FACE_VIS_NONE, &Scratch);
  Cases++;

  if (Failures == 0) {
    LogInfo("RENDER: face visibility self-test passed (%d cases)", Cases);
  } else {
    LogError("RENDER: face visibility self-test failed %d of %d cases",
             Failures, Cases);
  }
  return Failures == 0;
}
