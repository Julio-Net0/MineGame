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

#define VERTICES_PER_FACE CHUNK_VERTICES_PER_QUAD
#define OPAQUE_ALPHA_VALUE 255
#define ROUND_BIAS 0.5F

// Offset from a biome cell's corner to its centre block, resolved here rather
// than spelled inline so the division stays in integer context at its use sites.
enum {
  BIOME_CELL_CENTER = BIOME_CELL_SIZE / 2
};
#define CHUNK_CLOSE_DISTANCE_FACTOR 2.0F

// Sized to what the 16-bit shared index buffer can address (65532 vertices =
// 16383 four-vertex quads).
#define MAX_MESH_QUADS CHUNK_MAX_QUADS
#define MAX_MESH_VERTICES CHUNK_MAX_VERTICES

#define CHUNK_SPHERE_RADIUS 14.0F
#define FRUSTUM_DOT_THRESHOLD 0.4F

// Which of a chunk's three meshes a face belongs to.
//
// Solid cube faces and alpha-cutout flora used to share one buffer, because both
// want depth writes and depth testing on. They are separated because they want
// different fragment shaders: a shader containing `discard` costs the hardware
// its early depth rejection for the whole draw, and only the cutout geometry
// needs the test. Merged, every chunk carrying a single flower would forfeit
// early-Z across all of its solid geometry.
typedef enum {
  MESH_TARGET_OPAQUE = 0,
  MESH_TARGET_CUTOUT,
  MESH_TARGET_TRANSLUCENT,
  MESH_TARGET_COUNT
} MeshTarget;

// Backend-agnostic mesh-builder scratch for one target. Filled per chunk, then
// handed to the render backend as MeshData. Holds no renderer (Raylib) types.
typedef struct {
  ChunkVertex Vertices[MAX_MESH_VERTICES];
  int VCount;
} MeshScratch;

typedef struct {
  // All three sized alike. The cutout set will hold far less than the others in
  // practice, but a shared size keeps every emit path identical and removes a
  // class of bug where a helper indexes the wrong capacity.
  MeshScratch Sets[MESH_TARGET_COUNT];

  // Everything the passes read. Held here so it travels with the scratch when
  // meshing moves to the worker threads.
  ChunkSnapshot Snapshot;
} MesherState;

// Indexed per meshing thread. One slot today; the workers claim their own once
// meshing leaves the main thread, which is what the snapshot design exists to
// allow.
enum {
  MESHER_SLOT_COUNT = CHUNK_WORKER_MAX_THREADS
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
static bool MesherHasRoom(const MesherState *State, MeshTarget Target) {
  return (State->Sets[Target].VCount + VERTICES_PER_FACE) <= MAX_MESH_VERTICES;
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
                                   BlockID SelfId) {
    BlockID Id = SnapshotBlockAt(Snap, LocalX, LocalY, LocalZ);
    if (Id == 0) { return true; }
    if (Id == SelfId && GetBlockDef(SelfId)->IsTransparent) { return false; }
    return GetBlockDef(Id)->IsTransparent;
}

static bool IsSolidForAO(const ChunkSnapshot *Snap, int LocalX, int LocalY, int LocalZ) {
    BlockID Id = SnapshotBlockAt(Snap, LocalX, LocalY, LocalZ);
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
// Vertex packing & mesh builder primitives
// ---------------------------------------------------------------------------

enum {
    VERTEX_COORD_MASK = 31U,
    VERTEX_AO_MASK = 3U,
    VERTEX_NORMAL_MASK = 7U,
    VERTEX_LAYER_MASK = 4095U,
    VERTEX_OVERLAY_SLOT_MASK = 15U,
    VERTEX_NORMAL_CROSS = 6,

    VERTEX_SHIFT_Y = 5U,
    VERTEX_SHIFT_Z = 10U,
    VERTEX_SHIFT_U = 15U,
    VERTEX_SHIFT_V = 20U,
    VERTEX_SHIFT_AO = 25U,
    VERTEX_SHIFT_NORMAL = 27U,

    VERTEX_SHIFT_OVERLAY = 12U,
    VERTEX_SHIFT_TINT = 16U,

    RGB565_MAX_5 = 31U,
    RGB565_MAX_6 = 63U,
    RGB565_DIVISOR = 255U,
    RGB565_ROUND = 127U,
    RGB565_SHIFT_G = 5U,
    RGB565_SHIFT_R = 11U
};

static uint16_t ColorToRgb565(Color8 Color) {
    uint32_t R5 =
        (((uint32_t)Color.R * RGB565_MAX_5) + RGB565_ROUND) / RGB565_DIVISOR;
    uint32_t G6 =
        (((uint32_t)Color.G * RGB565_MAX_6) + RGB565_ROUND) / RGB565_DIVISOR;
    uint32_t B5 =
        (((uint32_t)Color.B * RGB565_MAX_5) + RGB565_ROUND) / RGB565_DIVISOR;
    return (uint16_t)((R5 << RGB565_SHIFT_R) | (G6 << RGB565_SHIFT_G) | B5);
}

static uint32_t PackVertexWord1(int TexLayer, int OverlaySlot,
                                uint16_t Tint565) {
    uint32_t Base = (uint32_t)TexLayer & VERTEX_LAYER_MASK;
    uint32_t Slot = (uint32_t)OverlaySlot & VERTEX_OVERLAY_SLOT_MASK;
    return Base | (Slot << VERTEX_SHIFT_OVERLAY) |
           ((uint32_t)Tint565 << VERTEX_SHIFT_TINT);
}

static ChunkVertex PackChunkVertex(int LocalX, int LocalY, int LocalZ,
                                   int TileU, int TileV, int Ao, int Normal,
                                   uint32_t Word1) {
    uint32_t Word0 =
        ((uint32_t)LocalX & VERTEX_COORD_MASK) |
        (((uint32_t)LocalY & VERTEX_COORD_MASK) << VERTEX_SHIFT_Y) |
        (((uint32_t)LocalZ & VERTEX_COORD_MASK) << VERTEX_SHIFT_Z) |
        (((uint32_t)TileU & VERTEX_COORD_MASK) << VERTEX_SHIFT_U) |
        (((uint32_t)TileV & VERTEX_COORD_MASK) << VERTEX_SHIFT_V) |
        (((uint32_t)Ao & VERTEX_AO_MASK) << VERTEX_SHIFT_AO) |
        (((uint32_t)Normal & VERTEX_NORMAL_MASK) << VERTEX_SHIFT_NORMAL);
    return (ChunkVertex){.Word0 = Word0, .Word1 = Word1};
}

// Rotating the four emitted quad vertices (V0, V1, V2, V3) -> (V1, V2, V3, V0)
// when FlipQuad is true makes the standard index split (0, 1, 2, 0, 2, 3) form
// triangles (V1, V2, V3) and (V1, V3, V0), which is the exact same diagonal and
// winding as the flipped index pattern (0, 1, 3, 1, 2, 3). Every quad in the
// engine can then share a single static index buffer.
static void EmitQuadVertices(MeshScratch *Set,
                             const ChunkVertex Corners[VERTICES_PER_FACE],
                             bool FlipQuad) {
    ChunkVertex *Out = &Set->Vertices[Set->VCount];
    if (FlipQuad) {
        Out[0] = Corners[1];
        Out[1] = Corners[2];
        Out[2] = Corners[3];
        Out[3] = Corners[0];
    } else {
        Out[0] = Corners[0];
        Out[1] = Corners[1];
        Out[2] = Corners[2];
        Out[3] = Corners[3];
    }
    Set->VCount += VERTICES_PER_FACE;
}

static void AddGreedyFaceToMeshBuilder(MesherState *State, MeshTarget Target,
                                       BlockFace Face,
                                       int Lx0, int Ly0, int Lz0,
                                       int W, int H,
                                       int TexIndex, int OverlaySlot,
                                       const int Ao[4], uint16_t Tint565,
                                       bool FlipQuad) {
    if (!MesherHasRoom(State, Target)) { return; }

    int Coords[VERTICES_PER_FACE][3];
    switch (Face) {
        case FACE_TOP:    // u=X, v=Z
            Coords[0][0] = Lx0;     Coords[0][1] = Ly0 + 1; Coords[0][2] = Lz0 + H;
            Coords[1][0] = Lx0 + W; Coords[1][1] = Ly0 + 1; Coords[1][2] = Lz0 + H;
            Coords[2][0] = Lx0 + W; Coords[2][1] = Ly0 + 1; Coords[2][2] = Lz0;
            Coords[3][0] = Lx0;     Coords[3][1] = Ly0 + 1; Coords[3][2] = Lz0;
            break;
        case FACE_BOTTOM: // u=X, v=Z (winding reversed)
            Coords[0][0] = Lx0;     Coords[0][1] = Ly0; Coords[0][2] = Lz0;
            Coords[1][0] = Lx0 + W; Coords[1][1] = Ly0; Coords[1][2] = Lz0;
            Coords[2][0] = Lx0 + W; Coords[2][1] = Ly0; Coords[2][2] = Lz0 + H;
            Coords[3][0] = Lx0;     Coords[3][1] = Ly0; Coords[3][2] = Lz0 + H;
            break;
        case FACE_FRONT:  // u=X, v=Y
            Coords[0][0] = Lx0;     Coords[0][1] = Ly0;     Coords[0][2] = Lz0 + 1;
            Coords[1][0] = Lx0 + W; Coords[1][1] = Ly0;     Coords[1][2] = Lz0 + 1;
            Coords[2][0] = Lx0 + W; Coords[2][1] = Ly0 + H; Coords[2][2] = Lz0 + 1;
            Coords[3][0] = Lx0;     Coords[3][1] = Ly0 + H; Coords[3][2] = Lz0 + 1;
            break;
        case FACE_BACK:   // u=X reversed, v=Y
            Coords[0][0] = Lx0 + W; Coords[0][1] = Ly0;     Coords[0][2] = Lz0;
            Coords[1][0] = Lx0;     Coords[1][1] = Ly0;     Coords[1][2] = Lz0;
            Coords[2][0] = Lx0;     Coords[2][1] = Ly0 + H; Coords[2][2] = Lz0;
            Coords[3][0] = Lx0 + W; Coords[3][1] = Ly0 + H; Coords[3][2] = Lz0;
            break;
        case FACE_LEFT:   // u=Z, v=Y
            Coords[0][0] = Lx0; Coords[0][1] = Ly0;     Coords[0][2] = Lz0;
            Coords[1][0] = Lx0; Coords[1][1] = Ly0;     Coords[1][2] = Lz0 + W;
            Coords[2][0] = Lx0; Coords[2][1] = Ly0 + H; Coords[2][2] = Lz0 + W;
            Coords[3][0] = Lx0; Coords[3][1] = Ly0 + H; Coords[3][2] = Lz0;
            break;
        case FACE_RIGHT:  // u=Z reversed, v=Y
            Coords[0][0] = Lx0 + 1; Coords[0][1] = Ly0;     Coords[0][2] = Lz0 + W;
            Coords[1][0] = Lx0 + 1; Coords[1][1] = Ly0;     Coords[1][2] = Lz0;
            Coords[2][0] = Lx0 + 1; Coords[2][1] = Ly0 + H; Coords[2][2] = Lz0;
            Coords[3][0] = Lx0 + 1; Coords[3][1] = Ly0 + H; Coords[3][2] = Lz0 + W;
            break;
    }

    int TileU[VERTICES_PER_FACE] = {0, W, W, 0};
    int TileV[VERTICES_PER_FACE] = {H, H, 0, 0};
    uint32_t Word1 = PackVertexWord1(TexIndex, OverlaySlot, Tint565);

    ChunkVertex Corners[VERTICES_PER_FACE];
    for (int Corner = 0; Corner < VERTICES_PER_FACE; Corner++) {
        Corners[Corner] = PackChunkVertex(
            Coords[Corner][0], Coords[Corner][1], Coords[Corner][2],
            TileU[Corner], TileV[Corner], Ao[Corner], (int)Face, Word1);
    }

    EmitQuadVertices(&State->Sets[Target], Corners, FlipQuad);
}

// ---------------------------------------------------------------------------
// FaceMask and greedy scan
// ---------------------------------------------------------------------------

typedef struct {
    BlockID BlockId;
    uint16_t TexIndex;
    uint16_t Tint565;
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
// Returns the overlay slot the vertex carries, not the texture layer.
static int GetFaceOverlay(const BlockType *Def, BlockFace Face) {
    if (Face == FACE_TOP || Face == FACE_BOTTOM) {
        return NO_OVERLAY_SLOT;
    }
    return Def->SideOverlaySlot;
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
    if (A->Tint565 != B->Tint565) { return false; }
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

                BlockID Id = SnapshotBlockAt(Snap, Lx, Ly, Lz);
                if (Id == 0) { continue; }

                BlockType *Def = GetBlockDef(Id);
                if (Def->IsTransparent) { continue; }

                int Nlpos[3] = { Lpos[0], Lpos[1], Lpos[2] };
                Nlpos[Fd->NormalAxis] += Fd->NormalDir;
                if (!IsNeighbourTransparent(Snap,
                                            Nlpos[0], Nlpos[1], Nlpos[2], Id)) { continue; }

                Mask[U][V].BlockId   = Id;
                Mask[U][V].TexIndex  = (uint16_t)GetFaceTex(Def, Fd->Face);
                Mask[U][V].FlipQuad  = ComputeFaceAO(Snap, Lx, Ly, Lz,
                                                     Fd->Face, Mask[U][V].Ao);
                Mask[U][V].Tint565 = ColorToRgb565(GetFaceTint(
                    Snap, (Snap->ChunkX * CHUNK_SIZE) + Lx,
                    (Snap->ChunkY * CHUNK_SIZE) + Ly,
                    (Snap->ChunkZ * CHUNK_SIZE) + Lz, Def, Fd->Face));
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

                // Chunk-local position of the first block in the merged region
                int FirstLpos[3];
                FirstLpos[Fd->NormalAxis] = Slice;
                FirstLpos[Fd->UAxis] = U0;
                FirstLpos[Fd->VAxis] = V0;

                // Every face in a merged rectangle shares a block id and a face
                // direction, so its overlay slot is the same throughout and
                // needs no place in the mask.
                int OverlaySlot =
                    GetFaceOverlay(GetBlockDef(Origin->BlockId), Fd->Face);

                AddGreedyFaceToMeshBuilder(State, MESH_TARGET_OPAQUE, Fd->Face,
                                           FirstLpos[0], FirstLpos[1],
                                           FirstLpos[2], W, H,
                                           Origin->TexIndex, OverlaySlot,
                                           Origin->Ao, Origin->Tint565,
                                           Origin->FlipQuad);

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
                BlockID Id = SnapshotBlockAt(Snap, X, Y, Z);
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
                uint16_t Tint565 = ColorToRgb565(
                    ComputeBlockTint(Snap, Wx, Wy, Wz, Def->Tint));

                if (IsNeighbourTransparent(Snap, X, Y+1, Z, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_TOP, Ao);
                    AddGreedyFaceToMeshBuilder(State, MESH_TARGET_TRANSLUCENT, FACE_TOP,
                                               X, Y, Z, 1, 1, Def->TexTop,
                                               GetFaceOverlay(Def, FACE_TOP),
                                               Ao, Tint565, Flip);
                }
                if (IsNeighbourTransparent(Snap, X, Y-1, Z, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_BOTTOM, Ao);
                    AddGreedyFaceToMeshBuilder(State, MESH_TARGET_TRANSLUCENT, FACE_BOTTOM,
                                               X, Y, Z, 1, 1, Def->TexBottom,
                                               GetFaceOverlay(Def, FACE_BOTTOM),
                                               Ao, Tint565, Flip);
                }
                if (IsNeighbourTransparent(Snap, X+1, Y, Z, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_RIGHT, Ao);
                    AddGreedyFaceToMeshBuilder(State, MESH_TARGET_TRANSLUCENT, FACE_RIGHT,
                                               X, Y, Z, 1, 1, Def->TexSide,
                                               GetFaceOverlay(Def, FACE_RIGHT),
                                               Ao, Tint565, Flip);
                }
                if (IsNeighbourTransparent(Snap, X-1, Y, Z, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_LEFT, Ao);
                    AddGreedyFaceToMeshBuilder(State, MESH_TARGET_TRANSLUCENT, FACE_LEFT,
                                               X, Y, Z, 1, 1, Def->TexSide,
                                               GetFaceOverlay(Def, FACE_LEFT),
                                               Ao, Tint565, Flip);
                }
                if (IsNeighbourTransparent(Snap, X, Y, Z+1, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_FRONT, Ao);
                    AddGreedyFaceToMeshBuilder(State, MESH_TARGET_TRANSLUCENT, FACE_FRONT,
                                               X, Y, Z, 1, 1, Def->TexSide,
                                               GetFaceOverlay(Def, FACE_FRONT),
                                               Ao, Tint565, Flip);
                }
                if (IsNeighbourTransparent(Snap, X, Y, Z-1, Id)) {
                    Flip = ComputeFaceAO(Snap, X, Y, Z, FACE_BACK, Ao);
                    AddGreedyFaceToMeshBuilder(State, MESH_TARGET_TRANSLUCENT, FACE_BACK,
                                               X, Y, Z, 1, 1, Def->TexSide,
                                               GetFaceOverlay(Def, FACE_BACK),
                                               Ao, Tint565, Flip);
                }
            }
        }
    }
}

// One flat quad of a cross billboard: four chunk-local integer corner
// positions, full-tile UVs, the block's tint in RGB565 and full brightness in
// AO (cross blocks carry no AO).
static void AddCrossQuad(MesherState *State, const int Verts[4][3],
                         int TexLayer, uint16_t Tint565) {
    if (!MesherHasRoom(State, MESH_TARGET_CUTOUT)) { return; }

    static const int TILE_U[VERTICES_PER_FACE] = {0, 1, 1, 0};
    static const int TILE_V[VERTICES_PER_FACE] = {1, 1, 0, 0};
    uint32_t Word1 = PackVertexWord1(TexLayer, NO_OVERLAY_SLOT, Tint565);

    ChunkVertex Corners[VERTICES_PER_FACE];
    for (int Corner = 0; Corner < VERTICES_PER_FACE; Corner++) {
        Corners[Corner] = PackChunkVertex(
            Verts[Corner][0], Verts[Corner][1], Verts[Corner][2],
            TILE_U[Corner], TILE_V[Corner], 0, VERTEX_NORMAL_CROSS, Word1);
    }

    EmitQuadVertices(&State->Sets[MESH_TARGET_CUTOUT], Corners, false);
}

// Per-block pass for cross billboards: two diagonals across the voxel, each
// emitted front and back so the quads are visible from either side regardless
// of face-culling state.
static void BuildCrossPass(const ChunkSnapshot *Snap, MesherState *State) {
    for (int X = 0; X < CHUNK_SIZE; X++) {
        for (int Y = 0; Y < CHUNK_SIZE; Y++) {
            for (int Z = 0; Z < CHUNK_SIZE; Z++) {
                BlockID Id = SnapshotBlockAt(Snap, X, Y, Z);
                if (Id == 0) { continue; }

                BlockType *Def = GetBlockDef(Id);
                if (Def->RenderType != BLOCK_RENDER_CROSS) { continue; }

                int Wx = (Snap->ChunkX * CHUNK_SIZE) + X;
                int Wy = (Snap->ChunkY * CHUNK_SIZE) + Y;
                int Wz = (Snap->ChunkZ * CHUNK_SIZE) + Z;
                uint16_t Tint565 = ColorToRgb565(
                    ComputeBlockTint(Snap, Wx, Wy, Wz, Def->Tint));
                int Layer = Def->TexTop;

                int X0 = X;
                int Y0 = Y;
                int Z0 = Z;
                int X1 = X0 + 1;
                int Y1 = Y0 + 1;
                int Z1 = Z0 + 1;

                // Diagonal A: (X0,Z0) -> (X1,Z1). Diagonal B: (X0,Z1) -> (X1,Z0).
                int DiagA[4][3] = {
                    {X0, Y0, Z0}, {X1, Y0, Z1}, {X1, Y1, Z1}, {X0, Y1, Z0}
                };
                int DiagArev[4][3] = {
                    {X1, Y0, Z1}, {X0, Y0, Z0}, {X0, Y1, Z0}, {X1, Y1, Z1}
                };
                int DiagB[4][3] = {
                    {X0, Y0, Z1}, {X1, Y0, Z0}, {X1, Y1, Z0}, {X0, Y1, Z1}
                };
                int DiagBrev[4][3] = {
                    {X1, Y0, Z0}, {X0, Y0, Z1}, {X0, Y1, Z1}, {X1, Y1, Z0}
                };

                AddCrossQuad(State, DiagA, Layer, Tint565);
                AddCrossQuad(State, DiagArev, Layer, Tint565);
                AddCrossQuad(State, DiagB, Layer, Tint565);
                AddCrossQuad(State, DiagBrev, Layer, Tint565);
            }
        }
    }
}

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

// The packed vertices a finished mesh actually used, copied out of the worker's
// scratch in a single allocation so the worker can move on to the next chunk
// instead of waiting for the upload.
typedef struct {
  ChunkVertex *Vertices;
  int VertexCount;
} MeshBufferSet;

typedef struct {
  // The slot the request was made for, plus the coordinate it held at the time.
  // The chunk is re-checked against the coordinate on completion, so a slot that
  // turned over despite the in-flight flag yields a discarded mesh rather than
  // geometry attached to whatever now lives there.
  Chunk *Target;

  MeshBufferSet Sets[MESH_TARGET_COUNT];

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
    Set->Vertices = NULL;
    Set->VertexCount = 0;
}

// Copy the used portion of one scratch set into a fresh allocation. Returns
// false only when the allocation fails, in which case the set is left empty and
// the mesh is simply not produced.
static bool CopyMeshBufferSet(MeshBufferSet *Set, int VertexCount,
                              const ChunkVertex *Vertices) {
    FreeMeshBufferSet(Set);
    if (VertexCount <= 0) {
        return true;
    }

    size_t Bytes = (size_t)VertexCount * sizeof(ChunkVertex);
    Set->Vertices = (ChunkVertex *)malloc(Bytes);
    if (Set->Vertices == NULL) {
        return false;
    }

    memcpy(Set->Vertices, Vertices, Bytes);
    Set->VertexCount = VertexCount;
    return true;
}

static MeshHandle UploadMeshBufferSet(const MeshBufferSet *Set) {
    if (Set->VertexCount == 0) {
        return MESH_HANDLE_INVALID;
    }
    MeshData Data = {
        .Vertices = Set->Vertices,
        .VertexCount = Set->VertexCount,
    };
    return RenderUploadMesh(&Data);
}

void UnloadChunkMesh(Chunk *ChunkVal) {
    if (ChunkVal->HasMesh) {
        RenderFreeMesh(ChunkVal->ChunkMesh);
        ChunkVal->ChunkMesh = MESH_HANDLE_INVALID;
        ChunkVal->HasMesh = false;
    }
    if (ChunkVal->HasCutoutMesh) {
        RenderFreeMesh(ChunkVal->CutoutMesh);
        ChunkVal->CutoutMesh = MESH_HANDLE_INVALID;
        ChunkVal->HasCutoutMesh = false;
    }
    if (ChunkVal->HasTranslucentMesh) {
        RenderFreeMesh(ChunkVal->TranslucentMesh);
        ChunkVal->TranslucentMesh = MESH_HANDLE_INVALID;
        ChunkVal->HasTranslucentMesh = false;
    }
}

bool RendererHasPendingMeshWork(void) {
    MeshJob *Jobs = GetMeshJobs();
    for (int Idx = 0; Idx < MESH_JOB_SLOT_COUNT; Idx++) {
        if (atomic_load_explicit(&Jobs[Idx].Stage, memory_order_acquire) !=
            MESH_JOB_FREE) {
            return true;
        }
    }
    return false;
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

    for (int Target = 0; Target < MESH_TARGET_COUNT; Target++) {
        State->Sets[Target].VCount = 0;
    }

    static const int FACE_DIRS_COUNT = (int)(sizeof(FACE_DIRS) / sizeof(FACE_DIRS[0]));
    for (int Fd = 0; Fd < FACE_DIRS_COUNT; Fd++) {
        BuildGreedyFacePass(Snap, &FACE_DIRS[Fd], State);
    }
    BuildTransparentFacePass(Snap, State);
    BuildCrossPass(Snap, State);

    PROFILE_END(MeshBuildStart, PROFILE_MESH_BUILD);

    // Copied out so the worker can take the next chunk instead of waiting for
    // the main thread to consume its scratch.
    for (int Target = 0; Target < MESH_TARGET_COUNT; Target++) {
        const MeshScratch *Set = &State->Sets[Target];
        (void)CopyMeshBufferSet(&Job->Sets[Target], Set->VCount, Set->Vertices);
    }

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

            ChunkVal->ChunkMesh =
                UploadMeshBufferSet(&Job->Sets[MESH_TARGET_OPAQUE]);
            ChunkVal->HasMesh = (ChunkVal->ChunkMesh != MESH_HANDLE_INVALID);

            ChunkVal->CutoutMesh =
                UploadMeshBufferSet(&Job->Sets[MESH_TARGET_CUTOUT]);
            ChunkVal->HasCutoutMesh =
                (ChunkVal->CutoutMesh != MESH_HANDLE_INVALID);

            ChunkVal->TranslucentMesh =
                UploadMeshBufferSet(&Job->Sets[MESH_TARGET_TRANSLUCENT]);
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

        for (int Target = 0; Target < MESH_TARGET_COUNT; Target++) {
            FreeMeshBufferSet(&Job->Sets[Target]);
        }
        Job->Target = NULL;
        atomic_store_explicit(&Job->Stage, MESH_JOB_FREE, memory_order_release);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Frustum culling + world draw
// ---------------------------------------------------------------------------

// The six planes of the view frustum, each (a, b, c, d) with the interior on the
// positive side.
enum {
    FRUSTUM_PLANE_COUNT = 6,
    // A plane is (a, b, c, d); a matrix row is four elements. Same width, and
    // the extraction below relies on that.
    PLANE_COMPONENTS = 4,
    MATRIX_ROWS = 4
};

typedef struct {
    float Planes[FRUSTUM_PLANE_COUNT][PLANE_COMPONENTS];
} RenderFrustum;

// Gribb-Hartmann: each plane is a sum or difference of two rows of the combined
// view-projection matrix. The matrix arrives column-major, so row i is elements
// i, 4+i, 8+i, 12+i.
//
// This replaces a dot product against a fixed threshold, which approximated a
// cone of roughly 66 degrees half-angle. That is both wider than the real
// frustum and the wrong shape for it -- a cone cannot represent a rectangular
// cross-section -- so chunks off the sides of the screen were being drawn.
static void BuildRenderFrustum(RenderFrustum *Out) {
    float M[RENDER_MATRIX_ELEMENTS];
    RenderGetViewProjection(M);

    // Column-major input, so element (Row, Column) sits at Column * MATRIX_ROWS
    // + Row. Transposed out here once rather than indexed that way six times.
    float Row[MATRIX_ROWS][PLANE_COMPONENTS];
    for (int R = 0; R < MATRIX_ROWS; R++) {
        for (int C = 0; C < PLANE_COMPONENTS; C++) {
            Row[R][C] = M[(C * MATRIX_ROWS) + R];
        }
    }

    // left, right, bottom, top, near, far. The last row plus or minus one of
    // the first three.
    static const int AXIS[FRUSTUM_PLANE_COUNT] = {0, 0, 1, 1, 2, 2};
    static const float SIGN[FRUSTUM_PLANE_COUNT] = {1.0F, -1.0F, 1.0F,
                                                    -1.0F, 1.0F, -1.0F};
    const int LAST_ROW = MATRIX_ROWS - 1;

    for (int P = 0; P < FRUSTUM_PLANE_COUNT; P++) {
        int A = AXIS[P];
        for (int C = 0; C < PLANE_COMPONENTS; C++) {
            Out->Planes[P][C] = Row[LAST_ROW][C] + (SIGN[P] * Row[A][C]);
        }

        // Normalised so the plane equation yields a true distance, which is what
        // lets the sphere test compare against the radius directly.
        float Nx = Out->Planes[P][0];
        float Ny = Out->Planes[P][1];
        float Nz = Out->Planes[P][2];
        float Len = sqrtf((Nx * Nx) + (Ny * Ny) + (Nz * Nz));
        if (Len > 0.0F) {
            for (int C = 0; C < PLANE_COMPONENTS; C++) {
                Out->Planes[P][C] /= Len;
            }
        }
    }
}

// Conservative: a chunk touching the frustum is drawn. The bounding sphere is
// kept rather than an axis-aligned box because a chunk is a cube of known size,
// the test is four multiplies per plane, and the few extra chunks a tighter box
// would reject are nothing beside the ones the cone was admitting.
static bool IsChunkVisible(const RenderFrustum *Frustum, const Chunk *ChunkVal) {
    float const HALFCHUNKSIZE = CHUNK_SIZE / 2.0F;
    float Cx = (float)(ChunkVal->ChunkX * CHUNK_SIZE) + HALFCHUNKSIZE;
    float Cy = (float)(ChunkVal->ChunkY * CHUNK_SIZE) + HALFCHUNKSIZE;
    float Cz = (float)(ChunkVal->ChunkZ * CHUNK_SIZE) + HALFCHUNKSIZE;

    for (int P = 0; P < FRUSTUM_PLANE_COUNT; P++) {
        float Distance = (Frustum->Planes[P][0] * Cx) +
                         (Frustum->Planes[P][1] * Cy) +
                         (Frustum->Planes[P][2] * Cz) + Frustum->Planes[P][3];
        if (Distance < -CHUNK_SPHERE_RADIUS) {
            return false;
        }
    }
    return true;
}

static int *GetRenderedChunkCounter(void) {
    static int Count = 0;
    return &Count;
}

int GetLastRenderedChunkCount(void) { return *GetRenderedChunkCounter(); }

// Back-to-front ordering for the translucent pass.
//
// Both orderings the frame needs come from these buckets. Alpha blending needs
// approximate depth ordering and the solid pass needs occluders to generally
// precede what they occlude; neither needs a total order. Bucketing is linear,
// where the comparison sort it replaces was superlinear over as many as
// MAX_ACTIVE_CHUNKS entries every frame.
//
// The bucket is the squared distance between the two chunks' grid coordinates,
// which is exact rather than quantised: squared distance is monotonic in
// distance, so ordering by it orders by distance, and only chunks genuinely
// equidistant on the grid tie. The earlier version took the world-space distance
// between chunk centres, took its square root and divided by the chunk width,
// which tied everything within a chunk-width of the same distance and cost a
// `sqrtf` per visible chunk per frame -- affordable when only chunks carrying a
// translucent mesh needed it, less so now that the solid pass wants every
// visible chunk ordered.
//
// The bucket count bounds the render distance this can express: with dx, dy and
// dz each up to r, the largest value is 3r^2, so 256 buckets cover a render
// distance of 9. Beyond that the clamp puts the far chunks in one bucket and
// they draw in arbitrary order relative to each other, which is degradation
// rather than breakage.
enum {
    CHUNK_DISTANCE_BUCKETS = 256
};

typedef struct {
    int Index;
    int Bucket;
} ChunkOrderEntry;

static int ChunkDistanceBucket(int DeltaX, int DeltaY, int DeltaZ) {
    int Bucket = (DeltaX * DeltaX) + (DeltaY * DeltaY) + (DeltaZ * DeltaZ);
    if (Bucket >= CHUNK_DISTANCE_BUCKETS) { Bucket = CHUNK_DISTANCE_BUCKETS - 1; }
    return Bucket;
}

static Vec3 GetChunkWorldOrigin(const Chunk *ChunkVal) {
    return (Vec3){
        (float)(ChunkVal->ChunkX * CHUNK_SIZE) - BLOCK_HALF_SIZE,
        (float)(ChunkVal->ChunkY * CHUNK_SIZE) - BLOCK_HALF_SIZE,
        (float)(ChunkVal->ChunkZ * CHUNK_SIZE) - BLOCK_HALF_SIZE,
    };
}

void DrawWorld(World *WorldVal, RenderCamera CameraVal) {
    PROFILE_BEGIN(DrawStart);
    if (GetDebugState()->Wireframe) { RenderSetWireframe(true); }

    // Once per frame rather than once per chunk. The previous test normalised
    // the camera forward vector inside itself, so it was recomputed for every
    // chunk on every pass.
    RenderFrustum Frustum;
    BuildRenderFrustum(&Frustum);

    static bool Visible[MAX_ACTIVE_CHUNKS];
    static ChunkOrderEntry VisibleChunks[MAX_ACTIVE_CHUNKS];
    static int NearToFar[MAX_ACTIVE_CHUNKS];
    static int SlotOrder[MAX_ACTIVE_CHUNKS];

    int BucketCounts[CHUNK_DISTANCE_BUCKETS] = {0};
    int VisibleCount = 0;
    int ChunkCount = WorldVal->ChunkCount;

    // The camera's own chunk, so the per-chunk work below is integer subtraction
    // rather than anything in world space.
    int CameraChunkX = (int)__builtin_floorf(CameraVal.Position.x / CHUNK_SIZE);
    int CameraChunkY = (int)__builtin_floorf(CameraVal.Position.y / CHUNK_SIZE);
    int CameraChunkZ = (int)__builtin_floorf(CameraVal.Position.z / CHUNK_SIZE);

    // Visibility and distance in one sweep. The bucket is what every pass sorts
    // on, so deriving it here costs three integer multiplies per visible chunk
    // per frame rather than one per chunk per pass.
    for (int IdxI = 0; IdxI < ChunkCount; IdxI++) {
        Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
        Visible[IdxI] = IsChunkVisible(&Frustum, ChunkVal);
        if (!Visible[IdxI]) { continue; }

        VisibleChunks[VisibleCount].Index = IdxI;
        VisibleChunks[VisibleCount].Bucket =
            ChunkDistanceBucket(ChunkVal->ChunkX - CameraChunkX,
                                ChunkVal->ChunkY - CameraChunkY,
                                ChunkVal->ChunkZ - CameraChunkZ);
        BucketCounts[VisibleChunks[VisibleCount].Bucket]++;
        SlotOrder[VisibleCount] = IdxI;
        VisibleCount++;
    }

    // Counting sort walked from the near bucket up, so the output runs nearest
    // to farthest. The translucent pass reads the same array backwards.
    int Offsets[CHUNK_DISTANCE_BUCKETS];
    int Running = 0;
    for (int B = 0; B < CHUNK_DISTANCE_BUCKETS; B++) {
        Offsets[B] = Running;
        Running += BucketCounts[B];
    }
    for (int IdxI = 0; IdxI < VisibleCount; IdxI++) {
        NearToFar[Offsets[VisibleChunks[IdxI].Bucket]++] = VisibleChunks[IdxI].Index;
    }

    // The solid and cutout passes are the ones the toggle governs; SlotOrder is
    // the visible chunks in the order the slots happen to sit in, which is what
    // they traversed before this change.
    const int *SolidOrder = GetDebugState()->SortChunks ? NearToFar : SlotOrder;

    int Rendered = 0;

    // Solid faces, nearest first, with the shader that keeps early depth
    // rejection. Order is what gives that rejection something to reject: an
    // occluded chunk drawn before its occluder is shaded and then overwritten.
    //
    // The ForceCutoutShader toggle routes them back through the cutout shader,
    // restoring the pre-split behaviour so the two can be measured against each
    // other.
    RenderBeginChunkPass(GetDebugState()->ForceCutoutShader ? CHUNK_PASS_CUTOUT
                                                            : CHUNK_PASS_OPAQUE);
    for (int IdxI = 0; IdxI < VisibleCount; IdxI++) {
        Chunk *ChunkVal = &WorldVal->Chunks[SolidOrder[IdxI]];
        if (!ChunkVal->HasMesh) { continue; }
        RenderDrawMesh(ChunkVal->ChunkMesh, GetChunkWorldOrigin(ChunkVal));
        Rendered++;
    }
    RenderEndChunkPass();

    // Alpha-cutout flora, which needs the test the opaque shader drops. Ordered
    // with the solid pass; it runs after it and tests against the depth it wrote.
    RenderBeginChunkPass(CHUNK_PASS_CUTOUT);
    for (int IdxI = 0; IdxI < VisibleCount; IdxI++) {
        Chunk *ChunkVal = &WorldVal->Chunks[SolidOrder[IdxI]];
        if (!ChunkVal->HasCutoutMesh) { continue; }
        RenderDrawMesh(ChunkVal->CutoutMesh, GetChunkWorldOrigin(ChunkVal));
    }
    RenderEndChunkPass();

    // Immediate-mode, and it binds its own shader and texture, so it stays
    // outside the chunk passes rather than being interleaved with them.
    if (GetDebugState()->ChunkBorders) {
        for (int IdxI = 0; IdxI < ChunkCount; IdxI++) {
            if (!Visible[IdxI]) { continue; }
            Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
            Vec3 Center = {
                (float)(ChunkVal->ChunkX * CHUNK_SIZE) + CHUNK_HALF_SIZE,
                (float)(ChunkVal->ChunkY * CHUNK_SIZE) + CHUNK_HALF_SIZE,
                (float)(ChunkVal->ChunkZ * CHUNK_SIZE) + CHUNK_HALF_SIZE,
            };
            RenderDrawChunkBorder(Center, CHUNK_SIZE);
        }
    }

    // Translucent pass, back to front -- the near-to-far ordering walked in
    // reverse. Alpha blending is order-dependent, so this pass stays sorted
    // whatever the solid pass is doing; defeating it would be a rendering fault
    // rather than a measurement.
    RenderBeginTranslucentPass();

    RenderBeginChunkPass(CHUNK_PASS_CUTOUT);
    for (int IdxI = VisibleCount - 1; IdxI >= 0; IdxI--) {
        Chunk *ChunkVal = &WorldVal->Chunks[NearToFar[IdxI]];
        if (!ChunkVal->HasTranslucentMesh) { continue; }
        RenderDrawMesh(ChunkVal->TranslucentMesh, GetChunkWorldOrigin(ChunkVal));
    }
    RenderEndChunkPass();

    RenderEndTranslucentPass();

    *GetRenderedChunkCounter() = Rendered;

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
