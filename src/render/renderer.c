#include "render/renderer.h"
#include "render/backend.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "world/chunk.h"
#include "world/chunk_snapshot.h"
#include "ui/debug.h"
#include "core/log.h"
#include "core/profiler.h"
#include "core/tint.h"
#include "core/vecmath.h"
#include "world/chunk_worker.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VERTICES_PER_FACE 4
#define FLOATS_PER_VERTEX 3
#define COLOR_CHANNELS 4
#define OPAQUE_ALPHA_VALUE 255
#define ROUND_BIAS 0.5F

// Offset from a biome cell's corner to its centre block, resolved here rather
// than spelled inline so the division stays in integer context at its use sites.
enum {
  BIOME_CELL_CENTER = BIOME_CELL_SIZE / 2
};
#define CHUNK_CLOSE_DISTANCE_FACTOR 2.0F
#define INDICES_PER_FACES 6

// Sized to what the index type can address, not to the theoretical face count.
// Indices are unsigned short, so no vertex beyond 65535 is referenceable; the
// buffers previously held 98304, roughly half of which could never be reached.
// That waste was tolerable as one global buffer and is not once there is one per
// meshing thread.
#define MAX_MESH_QUADS 16383
#define MAX_MESH_VERTICES (MAX_MESH_QUADS * VERTICES_PER_FACE)
#define MAX_MESH_INDICES (MAX_MESH_QUADS * INDICES_PER_FACES)

#define CHUNK_SPHERE_RADIUS 14.0F
#define FRUSTUM_DOT_THRESHOLD 0.4F

// Backend-agnostic mesh-builder scratch. Filled per chunk, then handed to the
// render backend as MeshData. Holds no renderer (Raylib) types.
typedef struct {
  float TempTexCoords[MAX_MESH_VERTICES * 2];
  float TempVertices[MAX_MESH_VERTICES * FLOATS_PER_VERTEX];
  unsigned short TempIndices[MAX_MESH_INDICES];
  unsigned char TempColors[MAX_MESH_VERTICES * COLOR_CHANNELS];
  float TempTexCoords2[MAX_MESH_VERTICES * 2];

  int VCount;
  int ICount;

  float TransTexCoords[MAX_MESH_VERTICES * 2];
  float TransVertices[MAX_MESH_VERTICES * FLOATS_PER_VERTEX];
  unsigned short TransIndices[MAX_MESH_INDICES];
  unsigned char TransColors[MAX_MESH_VERTICES * COLOR_CHANNELS];
  float TransTexCoords2[MAX_MESH_VERTICES * 2];

  int TransVCount;
  int TransICount;

  // Everything the passes read. Held here so it travels with the scratch when
  // meshing moves to the worker threads.
  ChunkSnapshot Snapshot;
} MesherState;

// Indexed per meshing thread. One slot today; the workers claim their own once
// meshing leaves the main thread, which is what the snapshot design exists to
// allow.
enum {
  MESHER_SLOT_COUNT = CHUNK_WORKER_THREAD_COUNT
};

static MesherState *GetMesherStateSlot(int Slot) {
  static MesherState States[MESHER_SLOT_COUNT];
  if (Slot < 0 || Slot >= MESHER_SLOT_COUNT) {
    Slot = 0;
  }
  return &States[Slot];
}

// Emitting past the buffer would wrap the 16-bit indices and produce garbage
// geometry silently. A chunk dense enough to reach this is pathological, so a
// truncated mesh is the right degradation — but it must be bounded rather than
// left to corrupt.
static bool MesherHasRoom(const MesherState *State, bool IsTrans) {
  int Used = IsTrans ? State->TransVCount : State->VCount;
  return (Used + VERTICES_PER_FACE) <= MAX_MESH_VERTICES;
}

void InitRenderer(void) { RenderBackendInit(); }

void CloseRenderer(void) { RenderBackendShutdown(); }

// ---------------------------------------------------------------------------
// Biome tint
// ---------------------------------------------------------------------------

static int FloorDivCells(int Value, int Divisor) {
    int Quotient = Value / Divisor;
    if ((Value % Divisor != 0) && ((Value < 0) != (Divisor < 0))) {
        Quotient--;
    }
    return Quotient;
}

static Color8 BilinearTint(Color8 T00, Color8 T10, Color8 T01, Color8 T11,
                            float Fx, float Fz) {
    float W00 = (1.0F - Fx) * (1.0F - Fz);
    float W10 = Fx * (1.0F - Fz);
    float W01 = (1.0F - Fx) * Fz;
    float W11 = Fx * Fz;

    Color8 Out;
    Out.R = (unsigned char)(((float)T00.R * W00) + ((float)T10.R * W10) +
                            ((float)T01.R * W01) + ((float)T11.R * W11) + ROUND_BIAS);
    Out.G = (unsigned char)(((float)T00.G * W00) + ((float)T10.G * W10) +
                            ((float)T01.G * W01) + ((float)T11.G * W11) + ROUND_BIAS);
    Out.B = (unsigned char)(((float)T00.B * W00) + ((float)T10.B * W10) +
                            ((float)T01.B * W01) + ((float)T11.B * W11) + ROUND_BIAS);
    Out.A = OPAQUE_ALPHA_VALUE;
    return Out;
}

// Tint for one block, interpolated across the four horizontal biome cells around
// it. No vertical interpolation: tint does not need it and it would triple the
// lookups.
//
// The interpolation weights need no explicit quantization. Cell centres sit at
// global block 4c+2, so an integer block position always lands on an exact
// quarter-step between two centres, and Fx/Fz can only ever be 0, 0.25, 0.5 or
// 0.75. Tint therefore takes a bounded set of values across a border by
// construction, which is what keeps greedy merging alive there, while inside a
// biome every block still resolves to the palette's exact declared colour.
static Color8 ComputeBlockTint(const ChunkSnapshot *Snap, int Wx, int Wy,
                                int Wz, TintSource Source) {
    if (Source == TINT_NONE) {
        return COLOR_WHITE;
    }

    // The block's own cell, used as the fallback when a neighbouring chunk is
    // absent, so the edge of the loaded world fades to the local colour instead
    // of snapping to a default biome.
    unsigned char LocalBiome = SnapshotLocalBiome(
        Snap, Wx - (Snap->ChunkX * CHUNK_SIZE),
        Wy - (Snap->ChunkY * CHUNK_SIZE),
        Wz - (Snap->ChunkZ * CHUNK_SIZE));

    int CellY = FloorDivCells(Wy, BIOME_CELL_SIZE);
    int LatX = FloorDivCells(Wx - BIOME_CELL_CENTER, BIOME_CELL_SIZE);
    int LatZ = FloorDivCells(Wz - BIOME_CELL_CENTER, BIOME_CELL_SIZE);

    unsigned char B00 = SnapshotBiomeCell(Snap, LatX, CellY, LatZ, LocalBiome);
    unsigned char B10 = SnapshotBiomeCell(Snap, LatX + 1, CellY, LatZ, LocalBiome);
    unsigned char B01 = SnapshotBiomeCell(Snap, LatX, CellY, LatZ + 1, LocalBiome);
    unsigned char B11 = SnapshotBiomeCell(Snap, LatX + 1, CellY, LatZ + 1, LocalBiome);

    // Interior fast path: one biome under all four corners makes the
    // interpolation an identity, so return the declared colour directly rather
    // than trusting float arithmetic to reproduce it byte-for-byte.
    if (B00 == B10 && B00 == B01 && B00 == B11) {
        return GetBiomeTint(B00, Source);
    }

    float Fx = (float)(Wx - BIOME_CELL_CENTER - (LatX * BIOME_CELL_SIZE)) /
               (float)BIOME_CELL_SIZE;
    float Fz = (float)(Wz - BIOME_CELL_CENTER - (LatZ * BIOME_CELL_SIZE)) /
               (float)BIOME_CELL_SIZE;

    return BilinearTint(GetBiomeTint(B00, Source), GetBiomeTint(B10, Source),
                        GetBiomeTint(B01, Source), GetBiomeTint(B11, Source),
                        Fx, Fz);
}

// The tint a face carries. What it ends up colouring is the shader's call: on a
// face naming a side overlay only the overlay's texels take it, otherwise the
// whole texel does.
//
// The bottom face is the exception. It samples the plain dirt tile with no
// overlay to scope the tint, so the tint would land on the whole face; dirt is
// not grass, so it gets white instead.
static Color8 GetFaceTint(const ChunkSnapshot *Snap, int Wx, int Wy,
                           int Wz, const BlockType *Def, BlockFace Face) {
    if (Def->Tint == TINT_GRASS && Face == FACE_BOTTOM) {
        return COLOR_WHITE;
    }
    return ComputeBlockTint(Snap, Wx, Wy, Wz, Def->Tint);
}

// ---------------------------------------------------------------------------
// AO helpers
// ---------------------------------------------------------------------------

static bool IsNeighbourTransparent(const ChunkSnapshot *Snap,
                                   int LocalX, int LocalY, int LocalZ,
                                   unsigned char SelfId) {
    unsigned char Id = SnapshotBlockAt(Snap, LocalX, LocalY, LocalZ);
    if (Id == 0) { return true; }
    if (Id == SelfId && GetBlockDef(SelfId)->IsTransparent) { return false; }
    return GetBlockDef(Id)->IsTransparent;
}

static bool IsSolidForAO(const ChunkSnapshot *Snap, int LocalX, int LocalY, int LocalZ) {
    unsigned char Id = SnapshotBlockAt(Snap, LocalX, LocalY, LocalZ);
    if (Id == 0) { return false; }
    return GetBlockDef(Id)->IsSolid;
}

static const int AO_OFFSETS[6][4][3][3] = {
    // FACE_TOP
    {
        { {-1,1,0}, {0,1,1},  {-1,1,1}  },
        { {1,1,0},  {0,1,1},  {1,1,1}   },
        { {1,1,0},  {0,1,-1}, {1,1,-1}  },
        { {-1,1,0}, {0,1,-1}, {-1,1,-1} }
    },
    // FACE_BOTTOM
    {
        { {-1,-1,0}, {0,-1,-1}, {-1,-1,-1} },
        { {1,-1,0},  {0,-1,-1}, {1,-1,-1}  },
        { {1,-1,0},  {0,-1,1},  {1,-1,1}   },
        { {-1,-1,0}, {0,-1,1},  {-1,-1,1}  }
    },
    // FACE_LEFT
    {
        { {-1,-1,0}, {-1,0,-1}, {-1,-1,-1} },
        { {-1,-1,0}, {-1,0,1},  {-1,-1,1}  },
        { {-1,1,0},  {-1,0,1},  {-1,1,1}   },
        { {-1,1,0},  {-1,0,-1}, {-1,1,-1}  }
    },
    // FACE_RIGHT
    {
        { {1,-1,0}, {1,0,1},  {1,-1,1}  },
        { {1,-1,0}, {1,0,-1}, {1,-1,-1} },
        { {1,1,0},  {1,0,-1}, {1,1,-1}  },
        { {1,1,0},  {1,0,1},  {1,1,1}   }
    },
    // FACE_FRONT
    {
        { {-1,0,1}, {0,-1,1}, {-1,-1,1} },
        { {1,0,1},  {0,-1,1}, {1,-1,1}  },
        { {1,0,1},  {0,1,1},  {1,1,1}   },
        { {-1,0,1}, {0,1,1},  {-1,1,1}  }
    },
    // FACE_BACK
    {
        { {1,0,-1},  {0,-1,-1}, {1,-1,-1}  },
        { {-1,0,-1}, {0,-1,-1}, {-1,-1,-1} },
        { {-1,0,-1}, {0,1,-1},  {-1,1,-1}  },
        { {1,0,-1},  {0,1,-1},  {1,1,-1}   }
    }
};

static const unsigned char AO_BRIGHTNESS[4] = {255, 210, 165, 120};

static bool ComputeFaceAO(const ChunkSnapshot *Snap,
                           int LocalX, int LocalY, int LocalZ, BlockFace Face, int Ao[4]) {
    for (int V = 0; V < 4; V++) {
        int S1x = LocalX + AO_OFFSETS[Face][V][0][0];
        int S1y = LocalY + AO_OFFSETS[Face][V][0][1];
        int S1z = LocalZ + AO_OFFSETS[Face][V][0][2];
        int S2x = LocalX + AO_OFFSETS[Face][V][1][0];
        int S2y = LocalY + AO_OFFSETS[Face][V][1][1];
        int S2z = LocalZ + AO_OFFSETS[Face][V][1][2];
        int Cx  = LocalX + AO_OFFSETS[Face][V][2][0];
        int Cy  = LocalY + AO_OFFSETS[Face][V][2][1];
        int Cz  = LocalZ + AO_OFFSETS[Face][V][2][2];

        bool S1 = IsSolidForAO(Snap, S1x, S1y, S1z);
        bool S2 = IsSolidForAO(Snap, S2x, S2y, S2z);
        bool Corner = IsSolidForAO(Snap, Cx, Cy, Cz);

        if (S1 && S2) { Ao[V] = 3; }
        else { Ao[V] = ((int)S1 + (int)S2 + (int)Corner); }
    }
    return (Ao[0] + Ao[2] > Ao[1] + Ao[3]);
}

// ---------------------------------------------------------------------------
// Mesh builder primitives
// ---------------------------------------------------------------------------

static void AddFaceIndices(MesherState *State, bool IsTrans, bool FlipQuad) {
    if (IsTrans) {
        if (FlipQuad) {
            State->TransIndices[State->TransICount++] = State->TransVCount + 0;
            State->TransIndices[State->TransICount++] = State->TransVCount + 1;
            State->TransIndices[State->TransICount++] = State->TransVCount + 3;
            State->TransIndices[State->TransICount++] = State->TransVCount + 1;
            State->TransIndices[State->TransICount++] = State->TransVCount + 2;
            State->TransIndices[State->TransICount++] = State->TransVCount + 3;
        } else {
            State->TransIndices[State->TransICount++] = State->TransVCount + 0;
            State->TransIndices[State->TransICount++] = State->TransVCount + 1;
            State->TransIndices[State->TransICount++] = State->TransVCount + 2;
            State->TransIndices[State->TransICount++] = State->TransVCount + 0;
            State->TransIndices[State->TransICount++] = State->TransVCount + 2;
            State->TransIndices[State->TransICount++] = State->TransVCount + 3;
        }
    } else {
        if (FlipQuad) {
            State->TempIndices[State->ICount++] = State->VCount + 0;
            State->TempIndices[State->ICount++] = State->VCount + 1;
            State->TempIndices[State->ICount++] = State->VCount + 3;
            State->TempIndices[State->ICount++] = State->VCount + 1;
            State->TempIndices[State->ICount++] = State->VCount + 2;
            State->TempIndices[State->ICount++] = State->VCount + 3;
        } else {
            State->TempIndices[State->ICount++] = State->VCount + 0;
            State->TempIndices[State->ICount++] = State->VCount + 1;
            State->TempIndices[State->ICount++] = State->VCount + 2;
            State->TempIndices[State->ICount++] = State->VCount + 0;
            State->TempIndices[State->ICount++] = State->VCount + 2;
            State->TempIndices[State->ICount++] = State->VCount + 3;
        }
    }
}

// Tint and AO travel separately in the vertex colour: RGB carries the face's
// biome tint, alpha carries per-vertex AO brightness. They are not
// pre-multiplied here because the tint applies per texel — the shader decides
// which texels take it — while AO applies to the whole face.
//
// The tint is one colour for the face (a merged quad carries a single tint by
// construction, since MasksCompatible refuses to merge across two) while AO
// still varies per vertex.
static void AddFaceColors(MesherState *State, const int Ao[4], Color8 Tint, bool IsTrans) {
    int ColIdx = (IsTrans ? State->TransVCount : State->VCount) * COLOR_CHANNELS;
    unsigned char *ColorsArray = IsTrans ? State->TransColors : State->TempColors;
    for (int IdxI = 0; IdxI < 4; IdxI++) {
        ColorsArray[ColIdx++] = Tint.R;
        ColorsArray[ColIdx++] = Tint.G;
        ColorsArray[ColIdx++] = Tint.B;
        ColorsArray[ColIdx++] = AO_BRIGHTNESS[Ao[IdxI]];
    }
}

// UV: raw tile-count coords [0..W, 0..H] — shader + array texture handles tiling
static void AddGreedyFaceTexCoords(MesherState *State, float UMax, float VMax, bool IsTrans) {
    int UvIdx = (IsTrans ? State->TransVCount : State->VCount) * 2;
    float *UvArray = IsTrans ? State->TransTexCoords : State->TempTexCoords;
    UvArray[UvIdx++] = 0.0F; UvArray[UvIdx++] = VMax;
    UvArray[UvIdx++] = UMax; UvArray[UvIdx++] = VMax;
    UvArray[UvIdx++] = UMax; UvArray[UvIdx++] = 0.0F;
    UvArray[UvIdx++] = 0.0F; UvArray[UvIdx++] = 0.0F;
}

// texcoords2.x is the base texture array layer; .y is the optional overlay layer
// composited over it, or NO_TEXTURE_OVERLAY when the face has none.
static void AddFaceTexLayer(MesherState *State, float Layer, float OverlayLayer, bool IsTrans) {
    int Tc2Idx = (IsTrans ? State->TransVCount : State->VCount) * 2;
    float *Tc2Array = IsTrans ? State->TransTexCoords2 : State->TempTexCoords2;
    for (int IdxI = 0; IdxI < 4; IdxI++) {
        Tc2Array[Tc2Idx + (IdxI * 2) + 0] = Layer;
        Tc2Array[Tc2Idx + (IdxI * 2) + 1] = OverlayLayer;
    }
}

// Greedy quad vertices for a W×H merged face.
static void AddGreedyFaceVertices(MesherState *State, BlockFace Face,
                                   int Wx0, int Wy0, int Wz0,
                                   int W, int H, bool IsTrans) {
    float *VArray = IsTrans ? State->TransVertices : State->TempVertices;
    int VIdx = (IsTrans ? State->TransVCount : State->VCount) * FLOATS_PER_VERTEX;

    float X0 = (float)Wx0 - BLOCK_HALF_SIZE;
    float Y0 = (float)Wy0 - BLOCK_HALF_SIZE;
    float Z0 = (float)Wz0 - BLOCK_HALF_SIZE;
    float Fw = (float)W;
    float Fh = (float)H;

    switch (Face) {
        case FACE_TOP:    // u=X, v=Z
            VArray[VIdx++] = X0;    VArray[VIdx++] = Y0+1.0F; VArray[VIdx++] = Z0+Fh;
            VArray[VIdx++] = X0+Fw; VArray[VIdx++] = Y0+1.0F; VArray[VIdx++] = Z0+Fh;
            VArray[VIdx++] = X0+Fw; VArray[VIdx++] = Y0+1.0F; VArray[VIdx++] = Z0;
            VArray[VIdx++] = X0;    VArray[VIdx++] = Y0+1.0F; VArray[VIdx++] = Z0;
            break;
        case FACE_BOTTOM: // u=X, v=Z (winding reversed)
            VArray[VIdx++] = X0;    VArray[VIdx++] = Y0; VArray[VIdx++] = Z0;
            VArray[VIdx++] = X0+Fw; VArray[VIdx++] = Y0; VArray[VIdx++] = Z0;
            VArray[VIdx++] = X0+Fw; VArray[VIdx++] = Y0; VArray[VIdx++] = Z0+Fh;
            VArray[VIdx++] = X0;    VArray[VIdx++] = Y0; VArray[VIdx++] = Z0+Fh;
            break;
        case FACE_FRONT:  // u=X, v=Y
            VArray[VIdx++] = X0;    VArray[VIdx++] = Y0;    VArray[VIdx++] = Z0+1.0F;
            VArray[VIdx++] = X0+Fw; VArray[VIdx++] = Y0;    VArray[VIdx++] = Z0+1.0F;
            VArray[VIdx++] = X0+Fw; VArray[VIdx++] = Y0+Fh; VArray[VIdx++] = Z0+1.0F;
            VArray[VIdx++] = X0;    VArray[VIdx++] = Y0+Fh; VArray[VIdx++] = Z0+1.0F;
            break;
        case FACE_BACK:   // u=X reversed, v=Y
            VArray[VIdx++] = X0+Fw; VArray[VIdx++] = Y0;    VArray[VIdx++] = Z0;
            VArray[VIdx++] = X0;    VArray[VIdx++] = Y0;    VArray[VIdx++] = Z0;
            VArray[VIdx++] = X0;    VArray[VIdx++] = Y0+Fh; VArray[VIdx++] = Z0;
            VArray[VIdx++] = X0+Fw; VArray[VIdx++] = Y0+Fh; VArray[VIdx++] = Z0;
            break;
        case FACE_LEFT:   // u=Z, v=Y
            VArray[VIdx++] = X0; VArray[VIdx++] = Y0;    VArray[VIdx++] = Z0;
            VArray[VIdx++] = X0; VArray[VIdx++] = Y0;    VArray[VIdx++] = Z0+Fw;
            VArray[VIdx++] = X0; VArray[VIdx++] = Y0+Fh; VArray[VIdx++] = Z0+Fw;
            VArray[VIdx++] = X0; VArray[VIdx++] = Y0+Fh; VArray[VIdx++] = Z0;
            break;
        case FACE_RIGHT:  // u=Z reversed, v=Y
            VArray[VIdx++] = X0+1.0F; VArray[VIdx++] = Y0;    VArray[VIdx++] = Z0+Fw;
            VArray[VIdx++] = X0+1.0F; VArray[VIdx++] = Y0;    VArray[VIdx++] = Z0;
            VArray[VIdx++] = X0+1.0F; VArray[VIdx++] = Y0+Fh; VArray[VIdx++] = Z0;
            VArray[VIdx++] = X0+1.0F; VArray[VIdx++] = Y0+Fh; VArray[VIdx++] = Z0+Fw;
            break;
    }
}

static void AddGreedyFaceToMeshBuilder(MesherState *State, BlockFace Face,
                                        int Wx0, int Wy0, int Wz0,
                                        int W, int H,
                                        int TexIndex, int OverlayIndex,
                                        const int Ao[4], Color8 Tint,
                                        bool FlipQuad, bool IsTrans) {
    if (!MesherHasRoom(State, IsTrans)) { return; }
    AddFaceIndices(State, IsTrans, FlipQuad);
    AddGreedyFaceTexCoords(State, (float)W, (float)H, IsTrans);
    AddFaceColors(State, Ao, Tint, IsTrans);
    AddFaceTexLayer(State, (float)TexIndex, (float)OverlayIndex, IsTrans);
    AddGreedyFaceVertices(State, Face, Wx0, Wy0, Wz0, W, H, IsTrans);

    if (IsTrans) { State->TransVCount += VERTICES_PER_FACE; }
    else { State->VCount += VERTICES_PER_FACE; }
}

// ---------------------------------------------------------------------------
// FaceMask and greedy scan
// ---------------------------------------------------------------------------

typedef struct {
    unsigned char BlockId;
    unsigned char TexIndex;
    Color8 Tint;
    int Ao[4];
    bool FlipQuad;
    bool Used;
} FaceMaskData;

typedef struct {
    BlockFace Face;
    int NormalAxis; // 0=X, 1=Y, 2=Z
    int NormalDir;  // +1 or -1
    int UAxis;
    int VAxis;
} FaceDir;

static const FaceDir FACE_DIRS[6] = {
    { FACE_TOP,    1, +1, 0, 2 },
    { FACE_BOTTOM, 1, -1, 0, 2 },
    { FACE_RIGHT,  0, +1, 2, 1 },
    { FACE_LEFT,   0, -1, 2, 1 },
    { FACE_FRONT,  2, +1, 0, 1 },
    { FACE_BACK,   2, -1, 0, 1 },
};

static int GetFaceTex(BlockType *Def, BlockFace Face) {
    if (Face == FACE_TOP) { return Def->TexTop; }
    if (Face == FACE_BOTTOM) { return Def->TexBottom; }
    return Def->TexSide;
}

// The side overlay wraps only the side faces: it is authored as the grass fringe
// that runs around a block, while the top and bottom sample their own tiles.
static int GetFaceOverlay(const BlockType *Def, BlockFace Face) {
    if (Face == FACE_TOP || Face == FACE_BOTTOM) {
        return NO_TEXTURE_OVERLAY;
    }
    return Def->TexSideOverlay;
}

static bool MasksCompatible(const FaceMaskData *A, const FaceMaskData *B) {
    if (A->BlockId == 0 || B->BlockId == 0) { return false; }
    if (A->Used || B->Used) { return false; }
    if (A->BlockId != B->BlockId) { return false; }
    if (A->TexIndex != B->TexIndex) { return false; }
    if (A->FlipQuad != B->FlipQuad) { return false; }
    // A merged quad carries one tint, so two faces may only merge when they
    // already agree on it — otherwise the rectangle would take one block's
    // colour and paint it across the whole span.
    if (A->Tint.R != B->Tint.R || A->Tint.G != B->Tint.G ||
        A->Tint.B != B->Tint.B) { return false; }
    for (int IdxI = 0; IdxI < VERTICES_PER_FACE; IdxI++) { if (A->Ao[IdxI] != B->Ao[IdxI]) { return false; } }
    return true;
}

static void BuildGreedyFacePass(const ChunkSnapshot *Snap, const FaceDir *Fd, MesherState *State) {
    for (int Slice = 0; Slice < CHUNK_SIZE; Slice++) {
        FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE];

        // Build mask for this slice
        for (int U = 0; U < CHUNK_SIZE; U++) {
            for (int V = 0; V < CHUNK_SIZE; V++) {
                Mask[U][V].BlockId = 0;
                Mask[U][V].Used = false;

                int Lpos[3];
                Lpos[Fd->NormalAxis] = Slice;
                Lpos[Fd->UAxis] = U;
                Lpos[Fd->VAxis] = V;
                int Lx = Lpos[0];
                int Ly = Lpos[1];
                int Lz = Lpos[2];

                unsigned char Id = SnapshotBlockAt(Snap, Lx, Ly, Lz);
                if (Id == 0) { continue; }

                BlockType *Def = GetBlockDef(Id);
                if (Def->IsTransparent) { continue; }

                int Nlpos[3] = { Lpos[0], Lpos[1], Lpos[2] };
                Nlpos[Fd->NormalAxis] += Fd->NormalDir;
                if (!IsNeighbourTransparent(Snap,
                                            Nlpos[0], Nlpos[1], Nlpos[2], Id)) { continue; }

                Mask[U][V].BlockId   = Id;
                Mask[U][V].TexIndex  = (unsigned char)GetFaceTex(Def, Fd->Face);
                Mask[U][V].FlipQuad  = ComputeFaceAO(Snap, Lx, Ly, Lz,
                                                     Fd->Face, Mask[U][V].Ao);
                Mask[U][V].Tint = GetFaceTint(
                    Snap, (Snap->ChunkX * CHUNK_SIZE) + Lx,
                    (Snap->ChunkY * CHUNK_SIZE) + Ly,
                    (Snap->ChunkZ * CHUNK_SIZE) + Lz, Def, Fd->Face);
            }
        }

        for (int U0 = 0; U0 < CHUNK_SIZE; U0++) {
            for (int V0 = 0; V0 < CHUNK_SIZE; V0++) {
                FaceMaskData *Origin = &Mask[U0][V0];
                if (Origin->BlockId == 0 || Origin->Used) { continue; }

                // Expand width (u direction)
                int W = 1;
                while (U0 + W < CHUNK_SIZE &&
                       MasksCompatible(Origin, &Mask[U0 + W][V0])) {
                    W++;
                }

                // Expand height (v direction)
                int H = 1;
                bool CanExpand = true;
                while (V0 + H < CHUNK_SIZE && CanExpand) {
                    for (int Ku = 0; Ku < W; Ku++) {
                        if (!MasksCompatible(Origin, &Mask[U0 + Ku][V0 + H])) {
                            CanExpand = false;
                            break;
                        }
                    }
                    if (CanExpand) {
                        H++;
                    }
                }

                // Compute world position of first block in merged region
                int FirstLpos[3];
                FirstLpos[Fd->NormalAxis] = Slice;
                FirstLpos[Fd->UAxis] = U0;
                FirstLpos[Fd->VAxis] = V0;
                int Wx0 = (Snap->ChunkX * CHUNK_SIZE) + FirstLpos[0];
                int Wy0 = (Snap->ChunkY * CHUNK_SIZE) + FirstLpos[1];
                int Wz0 = (Snap->ChunkZ * CHUNK_SIZE) + FirstLpos[2];

                // Every face in a merged rectangle shares a block id and a face
                // direction, so its overlay layer is the same throughout and
                // needs no place in the mask.
                int OverlayIndex =
                    GetFaceOverlay(GetBlockDef(Origin->BlockId), Fd->Face);

                AddGreedyFaceToMeshBuilder(State, Fd->Face, Wx0, Wy0, Wz0, W, H,
                                           Origin->TexIndex, OverlayIndex,
                                           Origin->Ao, Origin->Tint,
                                           Origin->FlipQuad, false);

                // Mark used
                for (int Ku = 0; Ku < W; Ku++) {
                    for (int Kv = 0; Kv < H; Kv++) {
                        Mask[U0 + Ku][V0 + Kv].Used = true;
                    }
                }
            }
        }
    }
}

// Per-face pass for transparent blocks (no greedy merging)
static void BuildTransparentFacePass(const ChunkSnapshot *Snap, MesherState *State) {
    for (int X = 0; X < CHUNK_SIZE; X++) {
        for (int Y = 0; Y < CHUNK_SIZE; Y++) {
            for (int Z = 0; Z < CHUNK_SIZE; Z++) {
                unsigned char Id = SnapshotBlockAt(Snap, X, Y, Z);
                if (Id == 0) { continue; }

                BlockType *Def = GetBlockDef(Id);
                if (!Def->IsTransparent) { continue; }
                // Cross blocks are billboards, not cubes: the dedicated cross
                // pass emits their geometry, so skip them here.
                if (Def->RenderType == BLOCK_RENDER_CROSS) { continue; }

                int Ao[4];
                bool Flip;

                int Wx = (Snap->ChunkX * CHUNK_SIZE) + X;
                int Wy = (Snap->ChunkY * CHUNK_SIZE) + Y;
                int Wz = (Snap->ChunkZ * CHUNK_SIZE) + Z;

                // This pass merges nothing, so tint applies straight to each
                // face with no mask compatibility involved. Computed once per
                // block: it does not vary by face for a foliage-tinted block.
                Color8 Tint = ComputeBlockTint(Snap, Wx, Wy, Wz,
                                               Def->Tint);

                if (IsNeighbourTransparent(Snap, X, Y+1, Z, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_TOP, Ao);
                    AddGreedyFaceToMeshBuilder(State, FACE_TOP, Wx, Wy, Wz, 1, 1,
                                               Def->TexTop,
                                               GetFaceOverlay(Def, FACE_TOP),
                                               Ao, Tint, Flip, true);
                }
                if (IsNeighbourTransparent(Snap, X, Y-1, Z, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_BOTTOM, Ao);
                    AddGreedyFaceToMeshBuilder(State, FACE_BOTTOM, Wx, Wy, Wz, 1, 1,
                                               Def->TexBottom,
                                               GetFaceOverlay(Def, FACE_BOTTOM),
                                               Ao, Tint, Flip, true);
                }
                if (IsNeighbourTransparent(Snap, X+1, Y, Z, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_RIGHT, Ao);
                    AddGreedyFaceToMeshBuilder(State, FACE_RIGHT, Wx, Wy, Wz, 1, 1,
                                               Def->TexSide,
                                               GetFaceOverlay(Def, FACE_RIGHT),
                                               Ao, Tint, Flip, true);
                }
                if (IsNeighbourTransparent(Snap, X-1, Y, Z, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_LEFT, Ao);
                    AddGreedyFaceToMeshBuilder(State, FACE_LEFT, Wx, Wy, Wz, 1, 1,
                                               Def->TexSide,
                                               GetFaceOverlay(Def, FACE_LEFT),
                                               Ao, Tint, Flip, true);
                }
                if (IsNeighbourTransparent(Snap, X, Y, Z+1, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_FRONT, Ao);
                    AddGreedyFaceToMeshBuilder(State, FACE_FRONT, Wx, Wy, Wz, 1, 1,
                                               Def->TexSide,
                                               GetFaceOverlay(Def, FACE_FRONT),
                                               Ao, Tint, Flip, true);
                }
                if (IsNeighbourTransparent(Snap, X, Y, Z-1, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_BACK, Ao);
                    AddGreedyFaceToMeshBuilder(State, FACE_BACK, Wx, Wy, Wz, 1, 1,
                                               Def->TexSide,
                                               GetFaceOverlay(Def, FACE_BACK),
                                               Ao, Tint, Flip, true);
                }
            }
        }
    }
}

// One flat quad of a cross billboard: four corner positions, full-tile UVs, the
// block's tint in RGB and full brightness in alpha (cross blocks carry no AO).
// Emitted into the OPAQUE builder, not the translucent one: flora is an
// alpha-cutout (the shader discards the transparent texels), so it wants depth
// writes and depth testing on. Routing it through the blended, depth-write-off
// translucent pass is what made distant flora draw over near geometry.
static void AddCrossQuad(MesherState *State, const float Verts[4][3], int TexLayer, Color8 Tint) {
    int AoFull[4] = {0, 0, 0, 0};
    if (!MesherHasRoom(State, false)) { return; }

    AddFaceIndices(State, false, false);
    AddGreedyFaceTexCoords(State, 1.0F, 1.0F, false);
    AddFaceColors(State, AoFull, Tint, false);
    AddFaceTexLayer(State, (float)TexLayer, (float)NO_TEXTURE_OVERLAY, false);

    int VIdx = State->VCount * FLOATS_PER_VERTEX;
    for (int IdxI = 0; IdxI < VERTICES_PER_FACE; IdxI++) {
        State->TempVertices[VIdx++] = Verts[IdxI][0];
        State->TempVertices[VIdx++] = Verts[IdxI][1];
        State->TempVertices[VIdx++] = Verts[IdxI][2];
    }
    State->VCount += VERTICES_PER_FACE;
}

// Per-block pass for cross billboards: two diagonals across the voxel, each
// emitted front and back so the quads are visible from either side regardless
// of face-culling state.
static void BuildCrossPass(const ChunkSnapshot *Snap, MesherState *State) {
    for (int X = 0; X < CHUNK_SIZE; X++) {
        for (int Y = 0; Y < CHUNK_SIZE; Y++) {
            for (int Z = 0; Z < CHUNK_SIZE; Z++) {
                unsigned char Id = SnapshotBlockAt(Snap, X, Y, Z);
                if (Id == 0) { continue; }

                BlockType *Def = GetBlockDef(Id);
                if (Def->RenderType != BLOCK_RENDER_CROSS) { continue; }

                int Wx = (Snap->ChunkX * CHUNK_SIZE) + X;
                int Wy = (Snap->ChunkY * CHUNK_SIZE) + Y;
                int Wz = (Snap->ChunkZ * CHUNK_SIZE) + Z;
                Color8 Tint = ComputeBlockTint(Snap, Wx, Wy, Wz,
                                               Def->Tint);
                int Layer = Def->TexTop;

                float X0 = (float)Wx - BLOCK_HALF_SIZE;
                float Y0 = (float)Wy - BLOCK_HALF_SIZE;
                float Z0 = (float)Wz - BLOCK_HALF_SIZE;
                float X1 = X0 + 1.0F;
                float Y1 = Y0 + 1.0F;
                float Z1 = Z0 + 1.0F;

                // Diagonal A: (X0,Z0) -> (X1,Z1). Diagonal B: (X0,Z1) -> (X1,Z0).
                float DiagA[4][3] = {
                    {X0, Y0, Z0}, {X1, Y0, Z1}, {X1, Y1, Z1}, {X0, Y1, Z0}
                };
                float DiagArev[4][3] = {
                    {X1, Y0, Z1}, {X0, Y0, Z0}, {X0, Y1, Z0}, {X1, Y1, Z1}
                };
                float DiagB[4][3] = {
                    {X0, Y0, Z1}, {X1, Y0, Z0}, {X1, Y1, Z0}, {X0, Y1, Z1}
                };
                float DiagBrev[4][3] = {
                    {X1, Y0, Z0}, {X0, Y0, Z1}, {X0, Y1, Z1}, {X1, Y1, Z0}
                };

                AddCrossQuad(State, DiagA, Layer, Tint);
                AddCrossQuad(State, DiagArev, Layer, Tint);
                AddCrossQuad(State, DiagB, Layer, Tint);
                AddCrossQuad(State, DiagBrev, Layer, Tint);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// BuildChunkMesh
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Mesh job pool
// ---------------------------------------------------------------------------
//
// A chunk mesh is produced in three steps on two kinds of thread:
//
//   main   claim a slot, capture the snapshot, queue the job
//   worker run the passes into its own scratch, copy the result into the slot
//   main   upload the result to the GPU, release the slot
//
// The snapshot is what makes the middle step safe off the main thread: the
// worker reads it and nothing else, so no chunk can change underneath it.
//
// The pool is bounded. When every slot is taken the main thread simply stops
// queueing, which throttles the workers to the rate the frame can upload at and
// keeps memory fixed however far they would otherwise run ahead.

enum {
  MESH_JOB_SLOT_COUNT = 24,

  MESH_JOB_FREE = 0,
  MESH_JOB_QUEUED = 1,
  MESH_JOB_BUILDING = 2,
  MESH_JOB_READY = 3
};

// The vertices a finished mesh actually used, copied out of the worker's scratch
// so the worker can move on to the next chunk instead of waiting for the upload.
//
// One allocation per array rather than one block carved into five. Carving would
// save four calls per mesh on a worker thread, where they cost the frame
// nothing, in exchange for hand-written offset and alignment arithmetic and a
// pointer cast the analyser rightly objects to. Not a trade worth making.
typedef struct {
  float *Vertices;
  float *TexCoords;
  float *TexLayers;
  unsigned short *Indices;
  unsigned char *Colors;
  int VertexCount;
  int IndexCount;
} MeshBufferSet;

typedef struct {
  // The slot the request was made for, plus the coordinate it held at the time.
  // The chunk is re-checked against the coordinate on completion, so a slot that
  // turned over despite the in-flight flag yields a discarded mesh rather than
  // geometry attached to whatever now lives there.
  Chunk *Target;

  MeshBufferSet Opaque;
  MeshBufferSet Translucent;

  atomic_int Stage;
  int ChunkX;
  int ChunkY;
  int ChunkZ;

  // Captured before queueing; read only by the worker.
  ChunkSnapshot Snapshot;
} MeshJob;

static MeshJob *GetMeshJobs(void) {
  static MeshJob Jobs[MESH_JOB_SLOT_COUNT];
  return Jobs;
}

static void FreeMeshBufferSet(MeshBufferSet *Set) {
    free(Set->Vertices);
    free(Set->TexCoords);
    free(Set->TexLayers);
    free(Set->Indices);
    free(Set->Colors);
    Set->Vertices = NULL;
    Set->TexCoords = NULL;
    Set->TexLayers = NULL;
    Set->Indices = NULL;
    Set->Colors = NULL;
    Set->VertexCount = 0;
    Set->IndexCount = 0;
}

// Copy the used portion of one scratch set into a fresh allocation. Returns
// false only when the allocation fails, in which case the set is left empty and
// the mesh is simply not produced.
static bool CopyMeshBufferSet(MeshBufferSet *Set, int VertexCount, int IndexCount,
                              const float *Vertices, const float *TexCoords,
                              const float *TexLayers,
                              const unsigned short *Indices,
                              const unsigned char *Colors) {
    FreeMeshBufferSet(Set);
    if (VertexCount <= 0 || IndexCount <= 0) {
        return true;
    }

    size_t PositionBytes = (size_t)VertexCount * FLOATS_PER_VERTEX * sizeof(float);
    size_t PairBytes = (size_t)VertexCount * 2 * sizeof(float);
    size_t IndexBytes = (size_t)IndexCount * sizeof(unsigned short);
    size_t ColorBytes = (size_t)VertexCount * COLOR_CHANNELS;

    Set->Vertices = (float *)malloc(PositionBytes);
    Set->TexCoords = (float *)malloc(PairBytes);
    Set->TexLayers = (float *)malloc(PairBytes);
    Set->Indices = (unsigned short *)malloc(IndexBytes);
    Set->Colors = (unsigned char *)malloc(ColorBytes);

    if (Set->Vertices == NULL || Set->TexCoords == NULL ||
        Set->TexLayers == NULL || Set->Indices == NULL || Set->Colors == NULL) {
        FreeMeshBufferSet(Set);
        return false;
    }

    memcpy(Set->Vertices, Vertices, PositionBytes);
    memcpy(Set->TexCoords, TexCoords, PairBytes);
    memcpy(Set->TexLayers, TexLayers, PairBytes);
    memcpy(Set->Indices, Indices, IndexBytes);
    memcpy(Set->Colors, Colors, ColorBytes);

    Set->VertexCount = VertexCount;
    Set->IndexCount = IndexCount;
    return true;
}

static MeshHandle UploadMeshBufferSet(const MeshBufferSet *Set) {
    if (Set->VertexCount == 0) {
        return MESH_HANDLE_INVALID;
    }
    MeshData Data = {
        .Vertices = Set->Vertices,
        .Indices = Set->Indices,
        .TexCoords = Set->TexCoords,
        .Colors = Set->Colors,
        .TexLayers = Set->TexLayers,
        .VertexCount = Set->VertexCount,
        .IndexCount = Set->IndexCount,
    };
    return RenderUploadMesh(&Data);
}

void UnloadChunkMesh(Chunk *ChunkVal) {
    if (ChunkVal->HasMesh) {
        RenderFreeMesh(ChunkVal->ChunkMesh);
        ChunkVal->ChunkMesh = MESH_HANDLE_INVALID;
        ChunkVal->HasMesh = false;
    }
    if (ChunkVal->HasTranslucentMesh) {
        RenderFreeMesh(ChunkVal->TranslucentMesh);
        ChunkVal->TranslucentMesh = MESH_HANDLE_INVALID;
        ChunkVal->HasTranslucentMesh = false;
    }
}

bool RendererHasMeshJobCapacity(void) {
    MeshJob *Jobs = GetMeshJobs();
    for (int Idx = 0; Idx < MESH_JOB_SLOT_COUNT; Idx++) {
        if (atomic_load_explicit(&Jobs[Idx].Stage, memory_order_acquire) ==
            MESH_JOB_FREE) {
            return true;
        }
    }
    return false;
}

int RendererQueueMeshJob(World *WorldVal, Chunk *ChunkVal) {
    // An empty chunk needs no mesh at all, so it is resolved here rather than
    // occupying a slot and a worker to produce nothing.
    if (ChunkVal->SolidBlockCount == 0) {
        UnloadChunkMesh(ChunkVal);
        ChunkVal->IsDirty = false;
        return -1;
    }

    MeshJob *Jobs = GetMeshJobs();
    for (int Idx = 0; Idx < MESH_JOB_SLOT_COUNT; Idx++) {
        int Expected = MESH_JOB_FREE;
        if (!atomic_compare_exchange_strong_explicit(
                &Jobs[Idx].Stage, &Expected, MESH_JOB_BUILDING,
                memory_order_acq_rel, memory_order_relaxed)) {
            continue;
        }

        MeshJob *Job = &Jobs[Idx];
        Job->Target = ChunkVal;
        Job->ChunkX = ChunkVal->ChunkX;
        Job->ChunkY = ChunkVal->ChunkY;
        Job->ChunkZ = ChunkVal->ChunkZ;

        // Captured here, on the only thread that creates, evicts and edits
        // chunks, so the gather races nothing and needs no lock.
        PROFILE_BEGIN(SnapshotStart);
        CaptureChunkSnapshot(WorldVal, ChunkVal, &Job->Snapshot);
        PROFILE_END(SnapshotStart, PROFILE_MESH_SNAPSHOT);

#ifdef MINEGAME_SNAPSHOT_VERIFY
        {
          int Mismatches = VerifyChunkSnapshot(WorldVal, ChunkVal, &Job->Snapshot);
          if (Mismatches > 0) {
            LogError("SNAPSHOT: chunk (%d,%d,%d) had %d mismatching cells",
                     ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ,
                     Mismatches);
          }
        }
#endif

        // Cleared only on completion, and only if the chunk was not edited in
        // the meantime, so an edit landing mid-build is not lost.
        ChunkVal->IsMeshing = true;

        atomic_store_explicit(&Job->Stage, MESH_JOB_QUEUED, memory_order_release);
        return Idx;
    }

    return -1;
}

// Release a slot claimed by RendererQueueMeshJob that never reached the work
// queue. Without this the slot would sit occupied forever and the chunk's
// in-flight flag would keep it from ever being queued again -- the chunk would
// silently never get a mesh.
void RendererAbandonMeshJob(int JobId) {
    if (JobId < 0 || JobId >= MESH_JOB_SLOT_COUNT) {
        return;
    }
    MeshJob *Job = &GetMeshJobs()[JobId];
    if (Job->Target != NULL) {
        Job->Target->IsMeshing = false;
    }
    Job->Target = NULL;
    atomic_store_explicit(&Job->Stage, MESH_JOB_FREE, memory_order_release);
}

// Runs on a worker thread. Touches only the job's own snapshot and the scratch
// belonging to the mesher slot it was given, so two workers never overlap.
void RendererBuildMeshJob(int JobId, int MesherSlot) {
    if (JobId < 0 || JobId >= MESH_JOB_SLOT_COUNT) {
        return;
    }

    MeshJob *Job = &GetMeshJobs()[JobId];
    int Expected = MESH_JOB_QUEUED;
    if (!atomic_compare_exchange_strong_explicit(&Job->Stage, &Expected,
                                                 MESH_JOB_BUILDING,
                                                 memory_order_acq_rel,
                                                 memory_order_relaxed)) {
        return;
    }

    MesherState *State = GetMesherStateSlot(MesherSlot);
    const ChunkSnapshot *Snap = &Job->Snapshot;

    PROFILE_BEGIN(MeshBuildStart);

    State->VCount = 0; State->ICount = 0;
    State->TransVCount = 0; State->TransICount = 0;

    static const int FACE_DIRS_COUNT = (int)(sizeof(FACE_DIRS) / sizeof(FACE_DIRS[0]));
    for (int Fd = 0; Fd < FACE_DIRS_COUNT; Fd++) {
        BuildGreedyFacePass(Snap, &FACE_DIRS[Fd], State);
    }
    BuildTransparentFacePass(Snap, State);
    BuildCrossPass(Snap, State);

    PROFILE_END(MeshBuildStart, PROFILE_MESH_BUILD);

    // Copied out so the worker can take the next chunk instead of waiting for
    // the main thread to consume its scratch.
    (void)CopyMeshBufferSet(&Job->Opaque, State->VCount, State->ICount,
                            State->TempVertices, State->TempTexCoords,
                            State->TempTexCoords2, State->TempIndices,
                            State->TempColors);
    (void)CopyMeshBufferSet(&Job->Translucent, State->TransVCount,
                            State->TransICount, State->TransVertices,
                            State->TransTexCoords, State->TransTexCoords2,
                            State->TransIndices, State->TransColors);

    atomic_store_explicit(&Job->Stage, MESH_JOB_READY, memory_order_release);
}

// Main thread. Uploads one finished mesh and releases its slot; returns false
// when nothing is ready.
bool RendererUploadFinishedMesh(void) {
    MeshJob *Jobs = GetMeshJobs();
    for (int Idx = 0; Idx < MESH_JOB_SLOT_COUNT; Idx++) {
        MeshJob *Job = &Jobs[Idx];
        if (atomic_load_explicit(&Job->Stage, memory_order_acquire) !=
            MESH_JOB_READY) {
            continue;
        }

        Chunk *ChunkVal = Job->Target;

        // The in-flight flag should have kept the slot, but a mesh describing a
        // position the chunk no longer occupies must never be uploaded onto
        // whatever took its place.
        bool StillOurs = (ChunkVal != NULL) && (ChunkVal->ChunkX == Job->ChunkX) &&
                         (ChunkVal->ChunkY == Job->ChunkY) &&
                         (ChunkVal->ChunkZ == Job->ChunkZ);

        if (StillOurs) {
            UnloadChunkMesh(ChunkVal);

            ChunkVal->ChunkMesh = UploadMeshBufferSet(&Job->Opaque);
            ChunkVal->HasMesh = (ChunkVal->ChunkMesh != MESH_HANDLE_INVALID);

            ChunkVal->TranslucentMesh = UploadMeshBufferSet(&Job->Translucent);
            ChunkVal->HasTranslucentMesh =
                (ChunkVal->TranslucentMesh != MESH_HANDLE_INVALID);

            // Only now, and only if nothing edited the chunk after the snapshot
            // was taken. Clearing unconditionally would leave a block the player
            // placed mid-build invisible until something else dirtied the chunk.
            if (!ChunkVal->DirtiedWhileMeshing) {
                ChunkVal->IsDirty = false;
            }
            ChunkVal->DirtiedWhileMeshing = false;
            ChunkVal->IsMeshing = false;
        }

        FreeMeshBufferSet(&Job->Opaque);
        FreeMeshBufferSet(&Job->Translucent);
        Job->Target = NULL;
        atomic_store_explicit(&Job->Stage, MESH_JOB_FREE, memory_order_release);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Frustum culling + world draw
// ---------------------------------------------------------------------------

bool IsChunkInFrustum(RenderCamera CameraVal, Chunk *ChunkVal) {
    float const HALFCHUNKSIZE = CHUNK_SIZE / 2.0F;
    Vec3 ChunkCenter = {
        (float)(ChunkVal->ChunkX * CHUNK_SIZE) + HALFCHUNKSIZE,
        (float)(ChunkVal->ChunkY * CHUNK_SIZE) + HALFCHUNKSIZE,
        (float)(ChunkVal->ChunkZ * CHUNK_SIZE) + HALFCHUNKSIZE,
    };
    Vec3 VecToChunk = Vec3Sub(ChunkCenter, CameraVal.Position);
    float Distance = Vec3Length(VecToChunk);
    if (Distance < CHUNK_SPHERE_RADIUS * CHUNK_CLOSE_DISTANCE_FACTOR) { return true; }
    Vec3 DirToChunk = Vec3Scale(VecToChunk, 1.0F / Distance);
    Vec3 CamForward = Vec3Normalize(Vec3Sub(CameraVal.Target, CameraVal.Position));
    float DotProduct = Vec3Dot(CamForward, DirToChunk);
    float SafeMargin = CHUNK_SPHERE_RADIUS / Distance;
    return DotProduct >= (FRUSTUM_DOT_THRESHOLD - SafeMargin);
}

typedef struct {
    int Index;
    float DistanceSq;
} ChunkDistance;

static void ShellSortChunks(ChunkDistance Array[], int Len) {
    for (int Gap = Len / 2; Gap > 0; Gap /= 2) {
        for (int IdxI = Gap; IdxI < Len; IdxI++) {
            ChunkDistance Temp = Array[IdxI];
            int IdxJ;
            for (IdxJ = IdxI; IdxJ >= Gap && Array[IdxJ - Gap].DistanceSq < Temp.DistanceSq; IdxJ -= Gap) {
                Array[IdxJ] = Array[IdxJ - Gap];
            }
            Array[IdxJ] = Temp;
        }
    }
}

void DrawWorld(World *WorldVal, RenderCamera CameraVal) {
    PROFILE_BEGIN(DrawStart);
    if (GetDebugState()->Wireframe) { RenderSetWireframe(true); }

    // Opaque pass
    for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
        Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
        if (!IsChunkInFrustum(CameraVal, ChunkVal)) { continue; }
        if (ChunkVal->HasMesh) { RenderDrawMesh(ChunkVal->ChunkMesh); }
        if (GetDebugState()->ChunkBorders) {
            Vec3 Center = {
                (float)(ChunkVal->ChunkX * CHUNK_SIZE) + CHUNK_HALF_SIZE,
                (float)(ChunkVal->ChunkY * CHUNK_SIZE) + CHUNK_HALF_SIZE,
                (float)(ChunkVal->ChunkZ * CHUNK_SIZE) + CHUNK_HALF_SIZE,
            };
            RenderDrawChunkBorder(Center, CHUNK_SIZE);
        }
    }

    // Translucent pass (back-to-front)
    RenderBeginTranslucentPass();

    static ChunkDistance VisibleChunks[MAX_ACTIVE_CHUNKS];
    int VisibleCount = 0;

    for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
        Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
        if (!ChunkVal->HasTranslucentMesh) { continue; }
        if (!IsChunkInFrustum(CameraVal, ChunkVal)) { continue; }
        float const H = CHUNK_SIZE / 2.0F;
        Vec3 Center = {
            (float)(ChunkVal->ChunkX * CHUNK_SIZE) + H,
            (float)(ChunkVal->ChunkY * CHUNK_SIZE) + H,
            (float)(ChunkVal->ChunkZ * CHUNK_SIZE) + H,
        };
        Vec3 V = Vec3Sub(Center, CameraVal.Position);
        VisibleChunks[VisibleCount].Index = IdxI;
        VisibleChunks[VisibleCount].DistanceSq = Vec3Dot(V, V);
        VisibleCount++;
    }

    ShellSortChunks(VisibleChunks, VisibleCount);
    for (int IdxI = 0; IdxI < VisibleCount; IdxI++) {
        RenderDrawMesh(WorldVal->Chunks[VisibleChunks[IdxI].Index].TranslucentMesh);
    }

    RenderEndTranslucentPass();

    if (GetDebugState()->Wireframe) { RenderSetWireframe(false); }
    PROFILE_END(DrawStart, PROFILE_WORLD_DRAW);
}

void DrawAABBDebug(World *WorldVal, Player *PlayerVal) {
    if (!GetDebugState()->Aabb) { return; }

    Vec3 BottomPoints[COLLISION_POINTS];
    Vec3 TopPoints[COLLISION_POINTS];
    Vec3 ShinPoints[COLLISION_POINTS];
    Vec3 FacePoints[COLLISION_POINTS];

    GetPlayerPoints(PlayerVal, (PointConfig){ .Radius = PlayerVal->Radius - VERTICAL_RADIUS_SHRINK, .YOffset = 0.0F, .Epsilon = -COLLISION_EPSILON }, BottomPoints);
    GetPlayerPoints(PlayerVal, (PointConfig){ .Radius = PlayerVal->Radius - VERTICAL_RADIUS_SHRINK, .YOffset = PlayerVal->Size.y, .Epsilon = COLLISION_EPSILON }, TopPoints);

    const float LATERALDEBUGRADIUS = PlayerVal->Radius + 0.01F;
    GetPlayerPoints(PlayerVal, (PointConfig){ .Radius = LATERALDEBUGRADIUS, .YOffset = LATERAL_Y_MARGIN, .Epsilon = 0.0F }, ShinPoints);
    GetPlayerPoints(PlayerVal, (PointConfig){ .Radius = LATERALDEBUGRADIUS, .YOffset = PlayerVal->Size.y, .Epsilon = 0.0F }, FacePoints);

    float Sz = PLAYER_DEBUG_AABB_SQUARES_SIZE;
    float Wz = PLAYER_DEBUG_AABB_WIRES_SIZE;

    for (int IdxI = 0; IdxI < COLLISION_POINTS; IdxI++) {
        RenderDrawDebugCube(BottomPoints[IdxI], Sz, false, IsPointSolid(WorldVal, BottomPoints[IdxI]));
        RenderDrawDebugCube(TopPoints[IdxI], Sz, false, IsPointSolid(WorldVal, TopPoints[IdxI]));
        RenderDrawDebugCube(ShinPoints[IdxI], Wz, true, IsPointSolid(WorldVal, ShinPoints[IdxI]));
        RenderDrawDebugCube(FacePoints[IdxI], Wz, true, IsPointSolid(WorldVal, FacePoints[IdxI]));
    }
}

#define SELECTION_MARKER_SIZE (BLOCK_SIZE + 0.02F)
#define SELECTION_BOX_MIDPOINT 0.5F

static const Color8 SELECTION_CORNER_A_COLOR = {230, 40, 40, 255};
static const Color8 SELECTION_CORNER_B_COLOR = {40, 90, 230, 255};
static const Color8 SELECTION_BOX_COLOR = {40, 210, 70, 255};
static const Color8 SELECTION_OFFSET_COLOR = {235, 215, 40, 255};

// Draw the live prefab-capture selection: corner A (red), corner B (blue), the
// full bounding box (green) once both are set, and the stamp anchor (yellow).
// All markers are drawn x-ray so they stay visible through solid blocks.
void DrawPrefabSelection(Player *PlayerVal) {
    Vec3 MarkerSize = {SELECTION_MARKER_SIZE, SELECTION_MARKER_SIZE,
                       SELECTION_MARKER_SIZE};

    if (PlayerVal->HasSelectionA) {
        RenderDrawWireBox(PlayerVal->SelectionA, MarkerSize,
                          SELECTION_CORNER_A_COLOR, true);
    }
    if (PlayerVal->HasSelectionB) {
        RenderDrawWireBox(PlayerVal->SelectionB, MarkerSize,
                          SELECTION_CORNER_B_COLOR, true);
    }

    if (PlayerVal->HasSelectionA && PlayerVal->HasSelectionB) {
        Vec3 CornerA = PlayerVal->SelectionA;
        Vec3 CornerB = PlayerVal->SelectionB;
        Vec3 BoxCenter = {(CornerA.x + CornerB.x) * SELECTION_BOX_MIDPOINT,
                          (CornerA.y + CornerB.y) * SELECTION_BOX_MIDPOINT,
                          (CornerA.z + CornerB.z) * SELECTION_BOX_MIDPOINT};
        Vec3 BoxSize = {fabsf(CornerA.x - CornerB.x) + BLOCK_SIZE,
                        fabsf(CornerA.y - CornerB.y) + BLOCK_SIZE,
                        fabsf(CornerA.z - CornerB.z) + BLOCK_SIZE};
        RenderDrawWireBox(BoxCenter, BoxSize, SELECTION_BOX_COLOR, true);
    }

    if (PlayerVal->HasSelectionOffset) {
        RenderDrawWireBox(PlayerVal->SelectionOffset, MarkerSize,
                          SELECTION_OFFSET_COLOR, true);
    }
}
