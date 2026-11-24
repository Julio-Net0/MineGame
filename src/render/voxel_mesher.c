#include "render/voxel_mesher.h"

#include "core/color.h"
#include "core/tint.h"
#include "render/chunk_visibility.h"
#include "render/renderer.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "world/cave_fields.h"
#include "world/chunk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VERTICES_PER_FACE CHUNK_VERTICES_PER_QUAD
#define OPAQUE_ALPHA_VALUE 255
#define ROUND_BIAS 0.5F

// Offset from a biome cell's corner to its centre block, resolved here rather
// than spelled inline so the division stays in integer context at its use sites.
enum {
  BIOME_CELL_CENTER = BIOME_CELL_SIZE / 2
};

// Sized to what the 16-bit shared index buffer can address (65532 vertices =
// 16383 four-vertex quads).
#define MAX_MESH_QUADS CHUNK_MAX_QUADS
#define MAX_MESH_VERTICES CHUNK_MAX_VERTICES

// Emitting past the buffer would wrap the 16-bit indices and produce garbage
// geometry silently. A chunk dense enough to reach this is pathological, so a
// truncated mesh is the right degradation -- but it must be bounded rather than
// left to corrupt.
static bool MesherHasRoom(VoxelMesher *State, MeshTarget Target) {
  bool Room = (State->Sets[Target].VCount + VERTICES_PER_FACE) <= MAX_MESH_VERTICES;
  if (!Room) {
    State->Truncated = true;
  }
  return Room;
}

// ---------------------------------------------------------------------------
// Face-culling classes
// ---------------------------------------------------------------------------
//
// A face of block P toward neighbour N is drawn when P emits cube faces, N lets
// faces through, and P is not a transparent block facing its own id (water
// against water). Air is decided by id: it emits nothing and passes every face.

enum {
    CULL_EMITS = 1U,
    CULL_PASSES = 2U,
    CULL_TRANSPARENT = 4U,

    CULL_PASSES_SHIFT = 1U,
    CULL_TRANSPARENT_SHIFT = 2U
};

// Resolved once from the registry, which is final before the renderer starts:
// the workers then classify a cell with a table read instead of a definition
// fetch.
typedef struct {
    uint8_t ById[BLOCK_REGISTRY_SIZE];
    // What GetBlockDef resolves an id past the registry to.
    uint8_t Unregistered;
} CullClassTable;

static CullClassTable *GetCullClassTable(void) {
    static CullClassTable Table;
    return &Table;
}

static uint8_t ClassifyBlockDef(const BlockType *Def) {
    uint8_t Class = 0;
    if (Def->IsTransparent) {
        Class |= (uint8_t)(CULL_PASSES | CULL_TRANSPARENT);
    }
    // Cross billboards are emitted by their own pass, not as cube faces.
    if (!Def->IsTransparent || Def->RenderType != BLOCK_RENDER_CROSS) {
        Class |= (uint8_t)CULL_EMITS;
    }
    return Class;
}

static void InitCullClassTable(void) {
    CullClassTable *Table = GetCullClassTable();
    for (int Id = 0; Id < BLOCK_REGISTRY_SIZE; Id++) {
        Table->ById[Id] = ClassifyBlockDef(GetBlockDef(Id));
    }
    // Air is decided by id, never by its definition: it emits nothing and
    // shows every face behind it.
    Table->ById[BLOCK_ID_AIR] = (uint8_t)CULL_PASSES;
    Table->Unregistered = ClassifyBlockDef(GetBlockDef(BLOCK_REGISTRY_SIZE));
}

static uint32_t CullClassOf(const CullClassTable *Table, BlockID Id) {
    return (Id < BLOCK_REGISTRY_SIZE) ? Table->ById[Id] : Table->Unregistered;
}


void VoxelMesherInit(void) { InitCullClassTable(); }

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
                                int Wz, const BlockType *Def) {
    TintSource Source = Def->Tint;
    if (Source == TINT_NONE) {
        return COLOR_WHITE;
    }
    // A constant tint is the block's own, the same in every biome, so there is
    // nothing to interpolate and merged faces stay uniform.
    if (Source == TINT_CONSTANT) {
        return Def->TintColor;
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
    return ComputeBlockTint(Snap, Wx, Wy, Wz, Def);
}

// ---------------------------------------------------------------------------
// AO helpers
// ---------------------------------------------------------------------------

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

static void AddGreedyFaceToMeshBuilder(VoxelMesher *State, MeshTarget Target,
                                       BlockFace Face,
                                       int Lx0, int Ly0, int Lz0,
                                       int W, int H,
                                       int TexIndex, int OverlaySlot,
                                       const int Ao[4], uint16_t Tint565,
                                       bool FlipQuad, uint32_t Flags) {
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
        Corners[Corner].Word0 |= Flags;
    }

    EmitQuadVertices(&State->Sets[Target], Corners, FlipQuad);
}

// ---------------------------------------------------------------------------
// FaceMask and greedy scan
// ---------------------------------------------------------------------------

// Valid only where the slice's row bitmask has the bit set: cells without a
// face are never read, so the mask is not cleared between slices.
typedef struct {
    BlockID BlockId;
    uint16_t TexIndex;
    uint16_t Tint565;
    int Ao[4];
    bool FlipQuad;
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

enum {
    // Snapshot bits 1..CHUNK_SIZE: the chunk itself, without the margin.
    CULL_INTERIOR_BITS = ((1U << CHUNK_SIZE) - 1U) << 1U,
    // A face column's bits, shifted down to chunk-local indices.
    CULL_LOCAL_BITS = (1U << CHUNK_SIZE) - 1U
};

// Classify every snapshot cell once and pack the classes into bit columns along
// Z and Y.
static void BuildCullColumns(const ChunkSnapshot *Snap, CullColumns *Cols) {
    const CullClassTable *Table = GetCullClassTable();
    memset(Cols->EmitsY, 0, sizeof(Cols->EmitsY));
    memset(Cols->PassesY, 0, sizeof(Cols->PassesY));
    memset(Cols->TransparentY, 0, sizeof(Cols->TransparentY));

    for (int X = 0; X < SNAPSHOT_BLOCK_SPAN; X++) {
        for (int Y = 0; Y < SNAPSHOT_BLOCK_SPAN; Y++) {
            const BlockID *Row = Snap->Blocks[X][Y];
            uint32_t Emits = 0;
            uint32_t Passes = 0;
            uint32_t Transparent = 0;
            for (int Z = 0; Z < SNAPSHOT_BLOCK_SPAN; Z++) {
                uint32_t Class = CullClassOf(Table, Row[Z]);
                uint32_t IsEmits = Class & CULL_EMITS;
                uint32_t IsPasses = (Class >> CULL_PASSES_SHIFT) & 1U;
                uint32_t IsTransparent = (Class >> CULL_TRANSPARENT_SHIFT) & 1U;
                Emits |= IsEmits << (uint32_t)Z;
                Passes |= IsPasses << (uint32_t)Z;
                Transparent |= IsTransparent << (uint32_t)Z;
                Cols->EmitsY[X][Z] |= IsEmits << (uint32_t)Y;
                Cols->PassesY[X][Z] |= IsPasses << (uint32_t)Y;
                Cols->TransparentY[X][Z] |= IsTransparent << (uint32_t)Y;
            }
            Cols->EmitsZ[X][Y] = Emits;
            Cols->PassesZ[X][Y] = Passes;
            Cols->TransparentZ[X][Y] = Transparent;
        }
    }
}

// The visible faces of one slice row as chunk-local bits along V. Each normal
// axis fixes its U and V axes in FACE_DIRS: Y faces run U=X, V=Z; X faces U=Z,
// V=Y; Z faces U=X, V=Y. Column bits are snapshot indices, so a chunk-local
// coordinate c is snapshot index c + 1.
static uint32_t FaceRowBits(const ChunkSnapshot *Snap, const CullColumns *Cols,
                            const FaceDir *Fd, int Slice, int U) {
    int Self = Slice + 1;
    int Next = Self + Fd->NormalDir;
    int Across = U + 1;

    uint32_t Faces;
    uint32_t SameIdCandidates;
    if (Fd->NormalAxis == 1) {
        Faces = Cols->EmitsZ[Across][Self] & Cols->PassesZ[Across][Next] &
                CULL_INTERIOR_BITS;
        SameIdCandidates = Faces & Cols->TransparentZ[Across][Self];
        while (SameIdCandidates != 0U) {
            int Bit = __builtin_ctz(SameIdCandidates);
            SameIdCandidates &= SameIdCandidates - 1U;
            if (Snap->Blocks[Across][Self][Bit] == Snap->Blocks[Across][Next][Bit]) {
                Faces &= ~(1U << (uint32_t)Bit);
            }
        }
    } else if (Fd->NormalAxis == 2) {
        Faces = Cols->EmitsY[Across][Self] & Cols->PassesY[Across][Next] &
                CULL_INTERIOR_BITS;
        SameIdCandidates = Faces & Cols->TransparentY[Across][Self];
        while (SameIdCandidates != 0U) {
            int Bit = __builtin_ctz(SameIdCandidates);
            SameIdCandidates &= SameIdCandidates - 1U;
            if (Snap->Blocks[Across][Bit][Self] == Snap->Blocks[Across][Bit][Next]) {
                Faces &= ~(1U << (uint32_t)Bit);
            }
        }
    } else {
        Faces = Cols->EmitsY[Self][Across] & Cols->PassesY[Next][Across] &
                CULL_INTERIOR_BITS;
        SameIdCandidates = Faces & Cols->TransparentY[Self][Across];
        while (SameIdCandidates != 0U) {
            int Bit = __builtin_ctz(SameIdCandidates);
            SameIdCandidates &= SameIdCandidates - 1U;
            if (Snap->Blocks[Self][Bit][Across] == Snap->Blocks[Next][Bit][Across]) {
                Faces &= ~(1U << (uint32_t)Bit);
            }
        }
    }
    return (Faces >> 1U) & CULL_LOCAL_BITS;
}

// Merging compares only face data: whether a cell holds an unused face at all
// is the row bitmask's business.
static bool MasksCompatible(const FaceMaskData *A, const FaceMaskData *B) {
    if (A->BlockId != B->BlockId) { return false; }
    if (A->TexIndex != B->TexIndex) { return false; }
    if (A->FlipQuad != B->FlipQuad) { return false; }
    // A merged quad carries one tint, so two faces may only merge when they
    // already agree on it -- otherwise the rectangle would take one block's
    // colour and paint it across the whole span.
    if (A->Tint565 != B->Tint565) { return false; }
    for (int IdxI = 0; IdxI < VERTICES_PER_FACE; IdxI++) { if (A->Ao[IdxI] != B->Ao[IdxI]) { return false; } }
    return true;
}

// One mask per slice holds the faces of both cube targets. Opaque and
// translucent blocks never share a block id, and merging requires equal ids, so
// a rectangle never mixes the two; its target follows from its block id at emit
// time. Rows[U] receives the slice's faces as bits along V. Returns how many
// faces the slice holds.
static unsigned char LodBiomeAt(const VoxelLodBiomes *Biomes, int X, int Y, int Z) {
    return Biomes->Cells[X + VOXEL_LOD_BIOME_MARGIN][Y][Z + VOXEL_LOD_BIOME_MARGIN];
}

// A level-1 voxel's biome-blended tint. Biome cells are 4 blocks wide and a
// level-1 voxel 2, aligned, so a voxel lies inside one cell column and all
// four of its blocks share the same four surrounding cells; the bilinear blend
// is separable, so the mean of the four blocks' chunk tints is the blend at
// their mean weights. A cell's biome is its voxels' (a voxel never straddles
// two cells).
static Color8 LodBlendedTint(const VoxelLodBiomes *Biomes, int Lx, int Ly, int Lz,
                             TintSource Source) {
    enum { VOXEL_BLOCKS = 2, CELL_VOXELS = BIOME_CELL_SIZE / VOXEL_BLOCKS };
    int Wx = Biomes->OriginX + (Lx * VOXEL_BLOCKS);
    int Wz = Biomes->OriginZ + (Lz * VOXEL_BLOCKS);
    int LatX = FloorDivCells(Wx - BIOME_CELL_CENTER, BIOME_CELL_SIZE);
    int LatZ = FloorDivCells(Wz - BIOME_CELL_CENTER, BIOME_CELL_SIZE);
    int Vx = (LatX * CELL_VOXELS) - (Biomes->OriginX / VOXEL_BLOCKS);
    int Vz = (LatZ * CELL_VOXELS) - (Biomes->OriginZ / VOXEL_BLOCKS);
    unsigned char B00 = LodBiomeAt(Biomes, Vx, Ly, Vz);
    unsigned char B10 = LodBiomeAt(Biomes, Vx + CELL_VOXELS, Ly, Vz);
    unsigned char B01 = LodBiomeAt(Biomes, Vx, Ly, Vz + CELL_VOXELS);
    unsigned char B11 = LodBiomeAt(Biomes, Vx + CELL_VOXELS, Ly, Vz + CELL_VOXELS);
    if (B00 == B10 && B00 == B01 && B00 == B11) {
        return GetBiomeTint(B00, Source);
    }
    float Fx = ((float)(Wx - BIOME_CELL_CENTER - (LatX * BIOME_CELL_SIZE)) + ROUND_BIAS) /
               (float)BIOME_CELL_SIZE;
    float Fz = ((float)(Wz - BIOME_CELL_CENTER - (LatZ * BIOME_CELL_SIZE)) + ROUND_BIAS) /
               (float)BIOME_CELL_SIZE;
    return BilinearTint(GetBiomeTint(B00, Source), GetBiomeTint(B10, Source),
                        GetBiomeTint(B01, Source), GetBiomeTint(B11, Source),
                        Fx, Fz);
}

// A LOD voxel's tint, with the same per-block rules a chunk face follows (no
// tint, a constant one, and white grass bottoms on opaque faces).
static Color8 LodFaceTint(const VoxelLodBiomes *Biomes, int Lx, int Ly, int Lz,
                          const BlockType *Def, BlockFace Face, bool Translucent) {
    if (Def->Tint == TINT_NONE) {
        return COLOR_WHITE;
    }
    if (Def->Tint == TINT_CONSTANT) {
        return Def->TintColor;
    }
    if (!Translucent && Def->Tint == TINT_GRASS && Face == FACE_BOTTOM) {
        return COLOR_WHITE;
    }
    if (Biomes->Blend) {
        return LodBlendedTint(Biomes, Lx, Ly, Lz, Def->Tint);
    }
    return GetBiomeTint((int)LodBiomeAt(Biomes, Lx, Ly, Lz), Def->Tint);
}

// Fills the mask cell of the face at (U, V) of a slice. Skirt faces stand
// against solid neighbours, so ambient occlusion would read them fully dark;
// they take none.
static void FillFaceCell(const ChunkSnapshot *Snap, const VoxelLodBiomes *LodBiomes,
                         const FaceDir *Fd, int Slice, int U, int V, bool Skirt,
                         FaceMaskData *Cell) {
    int Lpos[3];
    Lpos[Fd->NormalAxis] = Slice;
    Lpos[Fd->UAxis] = U;
    Lpos[Fd->VAxis] = V;
    int Lx = Lpos[0];
    int Ly = Lpos[1];
    int Lz = Lpos[2];

    BlockID Id = Snap->Blocks[Lx + 1][Ly + 1][Lz + 1];
    BlockType *Def = GetBlockDef(Id);
    bool Translucent = Def->IsTransparent;

    Cell->BlockId  = Id;
    Cell->TexIndex = (uint16_t)GetFaceTex(Def, Fd->Face);
    if (Skirt) {
        for (int Corner = 0; Corner < VERTICES_PER_FACE; Corner++) {
            Cell->Ao[Corner] = 0;
        }
        Cell->FlipQuad = false;
    } else {
        Cell->FlipQuad = ComputeFaceAO(Snap, Lx, Ly, Lz, Fd->Face, Cell->Ao);
    }

    int Wx = (Snap->ChunkX * CHUNK_SIZE) + Lx;
    int Wy = (Snap->ChunkY * CHUNK_SIZE) + Ly;
    int Wz = (Snap->ChunkZ * CHUNK_SIZE) + Lz;
    // Each target keeps the tint its faces had before translucent
    // merging existed: the translucent faces never took the grass
    // bottom-face exception GetFaceTint applies.
    Color8 Tint;
    if (LodBiomes != NULL) {
        Tint = LodFaceTint(LodBiomes, Lx, Ly, Lz, Def, Fd->Face, Translucent);
    } else {
        Tint = Translucent ? ComputeBlockTint(Snap, Wx, Wy, Wz, Def)
                           : GetFaceTint(Snap, Wx, Wy, Wz, Def, Fd->Face);
    }
    Cell->Tint565 = ColorToRgb565(Tint);
}

static int BuildSliceMask(const ChunkSnapshot *Snap, const CullColumns *Cols,
                          const VoxelLodBiomes *LodBiomes, const FaceDir *Fd,
                          int Slice, FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE],
                          uint16_t Rows[CHUNK_SIZE]) {
    int FaceCount = 0;
    for (int U = 0; U < CHUNK_SIZE; U++) {
        uint32_t Bits = FaceRowBits(Snap, Cols, Fd, Slice, U);
        Rows[U] = (uint16_t)Bits;
        while (Bits != 0U) {
            int V = __builtin_ctz(Bits);
            Bits &= Bits - 1U;
            FillFaceCell(Snap, LodBiomes, Fd, Slice, U, V, false, &Mask[U][V]);
            FaceCount++;
        }
    }
    return FaceCount;
}

// Blocks whose LOD faces carry VOXEL_LOD_FLAG_SEA: the sea and its frozen top.
static bool IsSeaSurfaceBlock(BlockID Id) {
    return Id != BLOCK_ID_AIR &&
           (Id == TerrainFluidBlock(CAVE_FLUID_WATER) || Id == TerrainIceBlockId());
}

// Whether the face at (U, V) is still unmerged and can join Origin's rectangle.
static bool CellMerges(const FaceMaskData *Origin,
                       FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE],
                       const uint16_t Avail[CHUNK_SIZE], int U, int V) {
    if ((((uint32_t)Avail[U] >> (uint32_t)V) & 1U) == 0U) { return false; }
    return MasksCompatible(Origin, &Mask[U][V]);
}

// Origins are taken in the order a full U-then-V walk would visit them: the
// lowest unmerged bit of a row is the next cell that walk would reach, since a
// rectangle only ever consumes cells at or past its own origin's V.
static void ScanGreedyMask(FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE],
                           uint16_t Avail[CHUNK_SIZE],
                           const FaceDir *Fd, int Slice, VoxelMesher *State,
                           uint32_t Flags) {
    for (int U0 = 0; U0 < CHUNK_SIZE; U0++) {
        while (Avail[U0] != 0U) {
            int V0 = __builtin_ctz(Avail[U0]);
            const FaceMaskData *Origin = &Mask[U0][V0];

            // Expand width (u direction)
            int W = 1;
            while (U0 + W < CHUNK_SIZE && CellMerges(Origin, Mask, Avail, U0 + W, V0)) {
                W++;
            }

            // Expand height (v direction)
            int H = 1;
            bool CanExpand = true;
            while (V0 + H < CHUNK_SIZE && CanExpand) {
                for (int Ku = 0; Ku < W; Ku++) {
                    if (!CellMerges(Origin, Mask, Avail, U0 + Ku, V0 + H)) {
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
            // direction, so its overlay slot and target are the same
            // throughout and need no place in the mask.
            const BlockType *Def = GetBlockDef(Origin->BlockId);
            MeshTarget Target = Def->IsTransparent ? MESH_TARGET_TRANSLUCENT
                                                   : MESH_TARGET_OPAQUE;
            // The LOD blends only what a viewer sees through; leaves stay
            // opaque (see VoxelMeshLodSection).
            uint32_t FaceFlags = Flags;
            if (State->LodBiomes != NULL) {
                if (Target == MESH_TARGET_TRANSLUCENT &&
                    !RenderBlockLayerSeeThrough(Origin->TexIndex)) {
                    Target = MESH_TARGET_OPAQUE;
                }
                if (IsSeaSurfaceBlock(Origin->BlockId)) {
                    FaceFlags |= VOXEL_LOD_FLAG_SEA;
                }
            }

            AddGreedyFaceToMeshBuilder(State, Target, Fd->Face,
                                       FirstLpos[0], FirstLpos[1],
                                       FirstLpos[2], W, H,
                                       Origin->TexIndex,
                                       GetFaceOverlay(Def, Fd->Face),
                                       Origin->Ao, Origin->Tint565,
                                       Origin->FlipQuad, FaceFlags);

            uint32_t Span = ((1U << (uint32_t)H) - 1U) << (uint32_t)V0;
            for (int Ku = 0; Ku < W; Ku++) {
                Avail[U0 + Ku] = (uint16_t)((uint32_t)Avail[U0 + Ku] & ~Span);
            }
        }
    }
}

// A skirt is the side face of a LOD voxel toward a solid neighbour within
// LOD_SKIRT_DEPTH voxels of the open cell above it, across a chunk column
// plane. The border pass cuts a section at those planes (every voxel in a 1:1
// column is discarded), and a covered voxel's top can stand above the real
// terrain it stands for (a level-1 voxel is two blocks tall): the face that
// closes the step from the open side was culled against that voxel, so
// without the skirt the view runs into the section's hollow interior and
// shows the sky. Only the border pass draws skirts, and only toward a 1:1
// column; anywhere else they would hide inside the neighbour's top voxel.
// See-through faces get none: they would lay a second water surface where
// the view already crossed the 1:1 one.
enum { LOD_SKIRT_DEPTH = 4 };

static uint32_t SkirtRowBits(const CullColumns *Cols, const FaceDir *Fd,
                             int Slice, int U) {
    int Self = Slice + 1;
    int Next = Self + Fd->NormalDir;
    int Across = U + 1;
    uint32_t SelfEmits;
    uint32_t NextPasses;
    if (Fd->NormalAxis == 0) {
        SelfEmits = Cols->EmitsY[Self][Across];
        NextPasses = Cols->PassesY[Next][Across];
    } else {
        SelfEmits = Cols->EmitsY[Across][Self];
        NextPasses = Cols->PassesY[Across][Next];
    }
    uint32_t NearOpen = 0;
    for (uint32_t Depth = 1; Depth <= (uint32_t)LOD_SKIRT_DEPTH; Depth++) {
        NearOpen |= NextPasses >> Depth;
    }
    uint32_t Skirts = SelfEmits & ~NextPasses & NearOpen & CULL_INTERIOR_BITS;
    return (Skirts >> 1U) & CULL_LOCAL_BITS;
}

// Whether the LOD draws this block's face blended (see ScanGreedyMask).
static bool LodFaceSeeThrough(BlockID Id, BlockFace Face) {
    BlockType *Def = GetBlockDef(Id);
    return Def->IsTransparent && RenderBlockLayerSeeThrough(GetFaceTex(Def, Face));
}

static void BuildLodSkirtPass(const ChunkSnapshot *Snap, const CullColumns *Cols,
                              const FaceDir *Fd, VoxelMesher *State) {
    FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE];
    uint16_t Rows[CHUNK_SIZE];
    for (int Slice = 0; Slice < CHUNK_SIZE; Slice++) {
        int Plane = (Fd->NormalDir > 0) ? Slice + 1 : Slice;
        if (Plane % State->LodSkirtPeriod != 0) {
            continue;
        }
        int FaceCount = 0;
        for (int U = 0; U < CHUNK_SIZE; U++) {
            uint32_t Bits = SkirtRowBits(Cols, Fd, Slice, U);
            uint32_t Kept = 0;
            while (Bits != 0U) {
                int V = __builtin_ctz(Bits);
                Bits &= Bits - 1U;
                int Lpos[3];
                Lpos[Fd->NormalAxis] = Slice;
                Lpos[Fd->UAxis] = U;
                Lpos[Fd->VAxis] = V;
                if (LodFaceSeeThrough(Snap->Blocks[Lpos[0] + 1][Lpos[1] + 1][Lpos[2] + 1],
                                      Fd->Face)) {
                    continue;
                }
                FillFaceCell(Snap, State->LodBiomes, Fd, Slice, U, V, true, &Mask[U][V]);
                Kept |= 1U << (uint32_t)V;
                FaceCount++;
            }
            Rows[U] = (uint16_t)Kept;
        }
        if (FaceCount > 0) {
            ScanGreedyMask(Mask, Rows, Fd, Slice, State, VOXEL_LOD_FLAG_SKIRT);
        }
    }
}

// Greedy merging for both cube targets. Translucent faces merge under the same
// rules as opaque ones; the translucent pass draws whole chunks back to front
// and never ordered the faces inside one, so merging changes no ordering.
// Skirts follow their direction's faces, so they share its culling run.
static void BuildGreedyFacePass(const ChunkSnapshot *Snap, const CullColumns *Cols,
                                const FaceDir *Fd, VoxelMesher *State) {
    FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE];
    uint16_t Rows[CHUNK_SIZE];
    for (int Slice = 0; Slice < CHUNK_SIZE; Slice++) {
        if (BuildSliceMask(Snap, Cols, State->LodBiomes, Fd, Slice, Mask,
                           Rows) > 0) {
            ScanGreedyMask(Mask, Rows, Fd, Slice, State, 0U);
        }
    }
    if (State->LodSkirtPeriod > 0 && Fd->NormalAxis != 1) {
        BuildLodSkirtPass(Snap, Cols, Fd, State);
    }
}

// The faces unloaded rock beside the chunk shows toward its open cells: a cave
// running out of the band's floor, or a cliff where a neighbour's band stops
// higher than this column's air. They lie on the chunk's boundary plane, as the
// rock's own chunk would have drawn them, and merge like any other face.
static void BuildRockCapPass(const ChunkSnapshot *Snap, const CullColumns *Cols,
                             VoxelMesher *State) {
    if (Snap->RockFaces == 0U) { return; }
    BlockID Stone = TerrainStoneBlockId();
    BlockType *Def = GetBlockDef(Stone);
    FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE];
    uint16_t Rows[CHUNK_SIZE];

    for (int Side = 0; Side < CHUNK_FACE_COUNT; Side++) {
        if ((Snap->RockFaces & (1U << (unsigned)Side)) == 0U) { continue; }
        int Axis = Side / 2;
        bool Positive = (Side % 2) == 1;
        // The rock's face toward the chunk points back along the axis.
        int NormalDir = Positive ? -1 : 1;
        const FaceDir *Fd = NULL;
        for (int Idx = 0; Idx < (int)(sizeof(FACE_DIRS) / sizeof(FACE_DIRS[0])); Idx++) {
            if (FACE_DIRS[Idx].NormalAxis == Axis && FACE_DIRS[Idx].NormalDir == NormalDir) {
                Fd = &FACE_DIRS[Idx];
            }
        }
        if (Fd == NULL) { continue; }
        int RockSlice = Positive ? CHUNK_SIZE : -1;
        int Inner = Positive ? CHUNK_SIZE : 1;

        for (int U = 0; U < CHUNK_SIZE; U++) {
            uint32_t Bits = 0;
            if (Axis == 1) {
                Bits = Cols->PassesZ[U + 1][Inner];
            } else if (Axis == 0) {
                Bits = Cols->PassesY[Inner][U + 1];
            } else {
                Bits = Cols->PassesY[U + 1][Inner];
            }
            Bits = (Bits >> 1U) & CULL_LOCAL_BITS;
            Rows[U] = (uint16_t)Bits;
            while (Bits != 0U) {
                int V = __builtin_ctz(Bits);
                Bits &= Bits - 1U;
                int Lpos[3];
                Lpos[Axis] = RockSlice;
                Lpos[Fd->UAxis] = U;
                Lpos[Fd->VAxis] = V;
                FaceMaskData *Cell = &Mask[U][V];
                Cell->BlockId = Stone;
                Cell->TexIndex = (uint16_t)GetFaceTex(Def, Fd->Face);
                Cell->FlipQuad = ComputeFaceAO(Snap, Lpos[0], Lpos[1], Lpos[2],
                                               Fd->Face, Cell->Ao);
                Color8 Tint = GetFaceTint(Snap, (Snap->ChunkX * CHUNK_SIZE) + Lpos[0],
                                          (Snap->ChunkY * CHUNK_SIZE) + Lpos[1],
                                          (Snap->ChunkZ * CHUNK_SIZE) + Lpos[2],
                                          Def, Fd->Face);
                Cell->Tint565 = ColorToRgb565(Tint);
            }
        }
        ScanGreedyMask(Mask, Rows, Fd, RockSlice, State, 0U);
    }
}

// One flat quad of a cross billboard: four chunk-local integer corner
// positions, full-tile UVs, the block's tint in RGB565 and full brightness in
// AO (cross blocks carry no AO).
static void AddCrossQuad(VoxelMesher *State, const int Verts[4][3],
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
static void BuildCrossPass(const ChunkSnapshot *Snap, VoxelMesher *State) {
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
                    ComputeBlockTint(Snap, Wx, Wy, Wz, Def));
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
// Entry points
// ---------------------------------------------------------------------------

static void ResetMesher(VoxelMesher *Mesher, const VoxelLodBiomes *LodBiomes) {
    for (int Target = 0; Target < MESH_TARGET_COUNT; Target++) {
        Mesher->Sets[Target].VCount = 0;
    }
    Mesher->LodBiomes = LodBiomes;
    Mesher->LodSkirtPeriod = 0;
    Mesher->Truncated = false;
}

static void BuildCubeFaces(VoxelMesher *Mesher, const ChunkSnapshot *Snap) {
    BuildCullColumns(Snap, &Mesher->Cull);
    static const int FACE_DIRS_COUNT = (int)(sizeof(FACE_DIRS) / sizeof(FACE_DIRS[0]));
    for (int Fd = 0; Fd < FACE_DIRS_COUNT; Fd++) {
        BuildGreedyFacePass(Snap, &Mesher->Cull, &FACE_DIRS[Fd], Mesher);
    }
}

void VoxelMeshChunk(VoxelMesher *Mesher, const ChunkSnapshot *Snap) {
    ResetMesher(Mesher, NULL);
    BuildCubeFaces(Mesher, Snap);
    BuildRockCapPass(Snap, &Mesher->Cull, Mesher);
    BuildCrossPass(Snap, Mesher);
}

enum {
    LOD_GRID_LAST = SNAPSHOT_BLOCK_SPAN - 1,
    LOD_EDGE = LOD_SECTION_SIZE - 1,
    LOD_FACES = 6
};

// The margin cells the neighbour at offset (Dx, Dy, Dz) touches: a face layer,
// an edge row or a corner cell. Along an axis with offset -1 the grid's cell 0
// takes the neighbour's last voxel, with +1 the grid's last cell its first,
// and with 0 the section's own span maps across. The edges and corners matter
// for AO: a face at a section's rim samples them, and left as air they lit the
// rim of every section (a grid of light lines under sea ice, which counts as
// solid for AO over the water beneath it).
static void FillMargin(ChunkSnapshot *Grid, const LodSection *Neighbour, int Dx,
                       int Dy, int Dz) {
    int Offset[3] = {Dx, Dy, Dz};
    int From[3];
    int Count[3];
    int Source[3];
    for (int Axis = 0; Axis < 3; Axis++) {
        Count[Axis] = 1;
        Source[Axis] = 0;
        if (Offset[Axis] < 0) {
            From[Axis] = 0;
            Source[Axis] = LOD_EDGE;
        } else if (Offset[Axis] > 0) {
            From[Axis] = LOD_GRID_LAST;
        } else {
            From[Axis] = 1;
            Count[Axis] = LOD_SECTION_SIZE;
        }
    }
    for (int X = 0; X < Count[0]; X++) {
        for (int Y = 0; Y < Count[1]; Y++) {
            for (int Z = 0; Z < Count[2]; Z++) {
                LodVoxel Voxel = LodSectionGet(Neighbour, Source[0] + X,
                                               Source[1] + Y, Source[2] + Z);
                Grid->Blocks[From[0] + X][From[1] + Y][From[2] + Z] =
                    LodVoxelBlock(Voxel);
            }
        }
    }
}

// A margin face toward a neighbour drawn at another level: seen-through
// blocks (water, ice, glass, leaves) find their own block there, so the two
// levels' seas and canopies meet without walls inside them, and everything
// else finds air, so the step between the levels' surfaces is closed by this
// section's wall instead of leaving a crack.
static void FillTransitionMargin(ChunkSnapshot *Grid, int Axis, bool Positive) {
    int GridLayer = Positive ? LOD_GRID_LAST : 0;
    int OwnLayer = Positive ? LOD_SECTION_SIZE : 1;
    for (int A = 1; A <= LOD_SECTION_SIZE; A++) {
        for (int B = 1; B <= LOD_SECTION_SIZE; B++) {
            int Target[3];
            int Own[3];
            Target[Axis] = GridLayer;
            Own[Axis] = OwnLayer;
            Target[(Axis + 1) % 3] = A;
            Own[(Axis + 1) % 3] = A;
            Target[(Axis + 2) % 3] = B;
            Own[(Axis + 2) % 3] = B;
            BlockID Id = Grid->Blocks[Own[0]][Own[1]][Own[2]];
            bool SeenThrough = Id != BLOCK_ID_AIR && GetBlockDef(Id)->IsTransparent;
            Grid->Blocks[Target[0]][Target[1]][Target[2]] =
                SeenThrough ? Id : (BlockID)BLOCK_ID_AIR;
        }
    }
}

// The biome margin side Face (0..3: -X, +X, -Z, +Z) from that neighbour: its
// MARGIN layers nearest the section, or the section's own edge repeated.
static void FillBiomeMargin(VoxelLodBiomes *Biomes, const LodSection *Neighbour,
                            int Face) {
    int Axis = (Face < 2) ? 0 : 2;
    bool Positive = (Face % 2) == 1;
    for (int Layer = 0; Layer < VOXEL_LOD_BIOME_MARGIN; Layer++) {
        int Target = Positive ? LOD_SECTION_SIZE + Layer : -1 - Layer;
        int Source = Positive ? Layer : LOD_SECTION_SIZE - 1 - Layer;
        int OwnEdge = Positive ? LOD_SECTION_SIZE - 1 : 0;
        for (int A = 0; A < LOD_SECTION_SIZE; A++) {
            for (int Y = 0; Y < LOD_SECTION_SIZE; Y++) {
                int Tx = (Axis == 0) ? Target : A;
                int Tz = (Axis == 0) ? A : Target;
                unsigned char Biome;
                if (Neighbour != NULL) {
                    int Sx = (Axis == 0) ? Source : A;
                    int Sz = (Axis == 0) ? A : Source;
                    Biome = (unsigned char)LodVoxelBiome(LodSectionGet(Neighbour, Sx, Y, Sz));
                } else {
                    Biome = (Axis == 0) ? LodBiomeAt(Biomes, OwnEdge, Y, A)
                                        : LodBiomeAt(Biomes, A, Y, OwnEdge);
                }
                Biomes->Cells[Tx + VOXEL_LOD_BIOME_MARGIN][Y][Tz + VOXEL_LOD_BIOME_MARGIN] = Biome;
            }
        }
    }
}

// Corners of the biome margin: the nearest -X/+X margin cell along Z.
static void FillBiomeCorners(VoxelLodBiomes *Biomes) {
    for (int X = 0; X < VOXEL_LOD_BIOME_SPAN; X++) {
        bool OutsideX = X < VOXEL_LOD_BIOME_MARGIN ||
                        X >= VOXEL_LOD_BIOME_MARGIN + LOD_SECTION_SIZE;
        if (!OutsideX) {
            continue;
        }
        for (int Z = 0; Z < VOXEL_LOD_BIOME_MARGIN; Z++) {
            for (int Y = 0; Y < LOD_SECTION_SIZE; Y++) {
                Biomes->Cells[X][Y][Z] = Biomes->Cells[X][Y][VOXEL_LOD_BIOME_MARGIN];
                Biomes->Cells[X][Y][VOXEL_LOD_BIOME_SPAN - 1 - Z] =
                    Biomes->Cells[X][Y][VOXEL_LOD_BIOME_MARGIN + LOD_SECTION_SIZE - 1];
            }
        }
    }
}

void VoxelMeshLodSection(VoxelMesher *Mesher, VoxelLodScratch *Scratch,
                         const LodSection *Section, const VoxelLodPlace *Place,
                         const LodSection *const Around[VOXEL_LOD_AROUND],
                         unsigned TransitionFaces) {
    // ChunkFace order: -X, +X, -Y, +Y, -Z, +Z.
    static const int FACE_OFFSETS[LOD_FACES][3] = {
        {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    ChunkSnapshot *Grid = &Scratch->Grid;
    // Air margins, no rock faces, a local origin: the grid stands alone.
    memset(Grid, 0, sizeof(*Grid));
    LodSectionDecode(Section, Scratch->Voxels);
    for (int X = 0; X < LOD_SECTION_SIZE; X++) {
        for (int Y = 0; Y < LOD_SECTION_SIZE; Y++) {
            for (int Z = 0; Z < LOD_SECTION_SIZE; Z++) {
                LodVoxel Voxel = Scratch->Voxels[LodVoxelIndex(X, Y, Z)];
                Grid->Blocks[X + 1][Y + 1][Z + 1] = LodVoxelBlock(Voxel);
                Scratch->Biomes.Cells[X + VOXEL_LOD_BIOME_MARGIN][Y]
                                     [Z + VOXEL_LOD_BIOME_MARGIN] =
                    (unsigned char)LodVoxelBiome(Voxel);
            }
        }
    }
    if (Around != NULL) {
        for (int Dx = -1; Dx <= 1; Dx++) {
            for (int Dy = -1; Dy <= 1; Dy++) {
                for (int Dz = -1; Dz <= 1; Dz++) {
                    const LodSection *Neighbour = Around[VoxelLodAround(Dx, Dy, Dz)];
                    if ((Dx != 0 || Dy != 0 || Dz != 0) && Neighbour != NULL) {
                        FillMargin(Grid, Neighbour, Dx, Dy, Dz);
                    }
                }
            }
        }
    }
    for (int Face = 0; Face < LOD_FACES; Face++) {
        if ((TransitionFaces & (1U << (unsigned)Face)) != 0U) {
            FillTransitionMargin(Grid, Face / 2, (Face % 2) == 1);
        }
    }
    // Margin faces in ChunkFace order: -X, +X, then (skipping Y) -Z, +Z.
    static const int SIDE_FACES[4] = {0, 1, 4, 5};
    for (int Side = 0; Side < 4; Side++) {
        const int *Offset = FACE_OFFSETS[SIDE_FACES[Side]];
        const LodSection *Neighbour =
            (Around != NULL) ? Around[VoxelLodAround(Offset[0], Offset[1], Offset[2])]
                             : NULL;
        FillBiomeMargin(&Scratch->Biomes, Neighbour, Side);
    }
    FillBiomeCorners(&Scratch->Biomes);
    int SectionBlocks = LOD_SECTION_SIZE << Place->Level;
    Scratch->Biomes.Blend = Place->Level == 1;
    Scratch->Biomes.OriginX = Place->X * SectionBlocks;
    Scratch->Biomes.OriginZ = Place->Z * SectionBlocks;
    ResetMesher(Mesher, &Scratch->Biomes);
    // Up to level 3 a voxel is narrower than a chunk, so the 1:1 columns cut
    // the section every CHUNK_SIZE >> Level voxels. Coarser sections reach the
    // 1:1 edge only where no finer data exists, and would pay skirts on every
    // voxel boundary.
    if (Place->Skirts && Place->Level >= 1 && Place->Level <= VOXEL_LOD_SKIRT_MAX_LEVEL) {
        Mesher->LodSkirtPeriod = CHUNK_SIZE >> Place->Level;
    }
    BuildCubeFaces(Mesher, Grid);
}
