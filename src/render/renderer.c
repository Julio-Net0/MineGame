#include "render/renderer.h"
#include "render/backend.h"
#include "render/chunk_visibility.h"
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
#include <limits.h>
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
// handed to the render backend as MeshData. Holds no graphics-API types.
typedef struct {
  ChunkVertex Vertices[MAX_MESH_VERTICES];
  int VCount;
} MeshScratch;

// Face-culling classes of every snapshot cell, packed as bit columns so the
// mesher finds a slice's visible faces with a few word operations instead of a
// block lookup per cell. Bit k of a column is snapshot index k along its axis
// (chunk-local k - 1), so the one-block margin sits in bits 0 and
// SNAPSHOT_BLOCK_SPAN - 1. Only the Z and Y axes are kept: every face direction
// lays its slice rows along one of them.
typedef struct {
  // Indexed [x][y], bits along z.
  uint32_t EmitsZ[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  uint32_t PassesZ[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  uint32_t TransparentZ[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  // Indexed [x][z], bits along y.
  uint32_t EmitsY[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  uint32_t PassesY[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  uint32_t TransparentY[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
} CullColumns;

typedef struct {
  // All three sized alike. The cutout set will hold far less than the others in
  // practice, but a shared size keeps every emit path identical and removes a
  // class of bug where a helper indexes the wrong capacity.
  MeshScratch Sets[MESH_TARGET_COUNT];

  // Everything the passes read. Held here so it travels with the scratch when
  // meshing moves to the worker threads.
  ChunkSnapshot Snapshot;

  FaceVisScratch Visibility;

  CullColumns Cull;
} MesherState;

// One per mesh worker, indexed by worker. Allocated for the actual worker
// count: each holds three full-chunk vertex buffers, so sizing them for the
// compiled maximum would cost tens of megabytes on a machine running a few
// workers.
typedef struct {
  MesherState *States;
  int Count;
} MesherSlots;

static MesherSlots *GetMesherSlots(void) {
  static MesherSlots Slots = {.States = (MesherState *)0, .Count = 0};
  return &Slots;
}

int RendererInitMesherSlots(int Count) {
  MesherSlots *Slots = GetMesherSlots();
  RendererFreeMesherSlots();
  // Halved on failure: fewer workers that can each mesh beats none starting.
  for (int Want = Count; Want > 0; Want /= 2) {
    MesherState *States =
        (MesherState *)malloc((size_t)Want * sizeof(MesherState));
    if (States != (MesherState *)0) {
      Slots->States = States;
      Slots->Count = Want;
      return Want;
    }
  }
  return 0;
}

void RendererFreeMesherSlots(void) {
  MesherSlots *Slots = GetMesherSlots();
  free(Slots->States);
  Slots->States = (MesherState *)0;
  Slots->Count = 0;
}

static MesherState *GetMesherStateSlot(int Slot) {
  MesherSlots *Slots = GetMesherSlots();
  if (Slot < 0 || Slot >= Slots->Count) {
    Slot = 0;
  }
  return &Slots->States[Slot];
}

// Emitting past the buffer would wrap the 16-bit indices and produce garbage
// geometry silently. A chunk dense enough to reach this is pathological, so a
// truncated mesh is the right degradation Ã¢â‚¬â€ but it must be bounded rather than
// left to corrupt.
static bool MesherHasRoom(const MesherState *State, MeshTarget Target) {
  return (State->Sets[Target].VCount + VERTICES_PER_FACE) <= MAX_MESH_VERTICES;
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

void InitRenderer(void) {
    RenderBackendInit();
    InitChunkVisibility();
    InitCullClassTable();
}

static void ResetRenderableSet(void);

void CloseRenderer(void) {
    ResetRenderableSet();
    RenderBackendShutdown();
    RendererFreeMesherSlots();
}

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
static int BuildSliceMask(const ChunkSnapshot *Snap, const CullColumns *Cols,
                          const FaceDir *Fd, int Slice,
                          FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE],
                          uint16_t Rows[CHUNK_SIZE]) {
    int FaceCount = 0;
    for (int U = 0; U < CHUNK_SIZE; U++) {
        uint32_t Bits = FaceRowBits(Snap, Cols, Fd, Slice, U);
        Rows[U] = (uint16_t)Bits;
        while (Bits != 0U) {
            int V = __builtin_ctz(Bits);
            Bits &= Bits - 1U;

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

            FaceMaskData *Cell = &Mask[U][V];
            Cell->BlockId  = Id;
            Cell->TexIndex = (uint16_t)GetFaceTex(Def, Fd->Face);
            Cell->FlipQuad = ComputeFaceAO(Snap, Lx, Ly, Lz, Fd->Face, Cell->Ao);

            int Wx = (Snap->ChunkX * CHUNK_SIZE) + Lx;
            int Wy = (Snap->ChunkY * CHUNK_SIZE) + Ly;
            int Wz = (Snap->ChunkZ * CHUNK_SIZE) + Lz;
            // Each target keeps the tint its faces had before translucent
            // merging existed: the translucent faces never took the grass
            // bottom-face exception GetFaceTint applies.
            Color8 Tint = Translucent
                              ? ComputeBlockTint(Snap, Wx, Wy, Wz, Def)
                              : GetFaceTint(Snap, Wx, Wy, Wz, Def, Fd->Face);
            Cell->Tint565 = ColorToRgb565(Tint);
            FaceCount++;
        }
    }
    return FaceCount;
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
                           const FaceDir *Fd, int Slice, MesherState *State) {
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

            AddGreedyFaceToMeshBuilder(State, Target, Fd->Face,
                                       FirstLpos[0], FirstLpos[1],
                                       FirstLpos[2], W, H,
                                       Origin->TexIndex,
                                       GetFaceOverlay(Def, Fd->Face),
                                       Origin->Ao, Origin->Tint565,
                                       Origin->FlipQuad);

            uint32_t Span = ((1U << (uint32_t)H) - 1U) << (uint32_t)V0;
            for (int Ku = 0; Ku < W; Ku++) {
                Avail[U0 + Ku] = (uint16_t)((uint32_t)Avail[U0 + Ku] & ~Span);
            }
        }
    }
}

// Greedy merging for both cube targets. Translucent faces merge under the same
// rules as opaque ones; the translucent pass draws whole chunks back to front
// and never ordered the faces inside one, so merging changes no ordering.
static void BuildGreedyFacePass(const ChunkSnapshot *Snap, const CullColumns *Cols,
                                const FaceDir *Fd, MesherState *State) {
    FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE];
    uint16_t Rows[CHUNK_SIZE];
    for (int Slice = 0; Slice < CHUNK_SIZE; Slice++) {
        if (BuildSliceMask(Snap, Cols, Fd, Slice, Mask, Rows) > 0) {
            ScanGreedyMask(Mask, Rows, Fd, Slice, State);
        }
    }
}

// The faces unloaded rock beside the chunk shows toward its open cells: a cave
// running out of the band's floor, or a cliff where a neighbour's band stops
// higher than this column's air. They lie on the chunk's boundary plane, as the
// rock's own chunk would have drawn them, and merge like any other face.
static void BuildRockCapPass(const ChunkSnapshot *Snap, const CullColumns *Cols,
                             MesherState *State) {
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
        ScanGreedyMask(Mask, Rows, Fd, RockSlice, State);
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
//
// Sized so the workers stay busy for a whole loading frame: at the maximum
// render distance a frame lasts several milliseconds against ~0.27 ms per mesh,
// and 24 slots let the pool go idle after a few hundred microseconds. The slot
// count also caps the uploads of one frame, since only finished slots upload:
// at R=64 a teleport settled in 1308 frames with 128 slots and in 510 with 256,
// while 512 tripled the frame p99 for little more. Each slot carries a ~12 KB
// snapshot, so 256 of them cost ~3 MB. Keep MAX_MESHES_PER_FRAME (main.c) equal
// to this: a higher ceiling there can never be reached.

enum {
  MESH_JOB_SLOT_COUNT = 256,

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

  // Written by the worker with the meshes, committed with them on upload.
  FaceVisibility Visibility;
  OpaqueLayerMask OpaqueLayers;

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

static Vec3 GetChunkWorldOrigin(const Chunk *ChunkVal) {
    return (Vec3){
        (float)(ChunkVal->ChunkX * CHUNK_SIZE) - BLOCK_HALF_SIZE,
        (float)(ChunkVal->ChunkY * CHUNK_SIZE) - BLOCK_HALF_SIZE,
        (float)(ChunkVal->ChunkZ * CHUNK_SIZE) - BLOCK_HALF_SIZE,
    };
}

static MeshHandle UploadMeshBufferSet(const MeshBufferSet *Set, Vec3 Origin) {
    if (Set->VertexCount == 0) {
        return MESH_HANDLE_INVALID;
    }
    MeshData Data = {
        .Vertices = Set->Vertices,
        .VertexCount = Set->VertexCount,
        .Origin = Origin,
    };
    return RenderUploadMesh(&Data);
}

// ---------------------------------------------------------------------------
// Chunk visibility
// ---------------------------------------------------------------------------

// Face connectivity per chunk slot. Kept here rather than on Chunk or in the
// renderable set: records exist only for chunks with geometry, and the
// occlusion traversal has to cross air and buried chunks that have none.
typedef struct {
    const Chunk *Base;
    FaceVisibility Slots[MAX_ACTIVE_CHUNKS];
    // Fully opaque layers per slot, committed with the mesh they were computed
    // with, so an edit never makes the horizon cull trust a wall that the
    // geometry on screen no longer draws (or the reverse).
    OpaqueLayerMask Layers[MAX_ACTIVE_CHUNKS];
} ChunkVisibilityTable;

static ChunkVisibilityTable *GetChunkVisibilityTable(void) {
    static ChunkVisibilityTable Table;
    return &Table;
}

void RendererAttachWorld(const World *WorldVal) {
    GetChunkVisibilityTable()->Base = WorldVal->Chunks;
}

static int ChunkVisibilitySlot(const Chunk *ChunkVal) {
    const ChunkVisibilityTable *Table = GetChunkVisibilityTable();
    if (Table->Base == NULL) { return -1; }
    ptrdiff_t Offset = ChunkVal - Table->Base;
    if (Offset < 0 || Offset >= (ptrdiff_t)MAX_ACTIVE_CHUNKS) { return -1; }
    return (int)Offset;
}

static void SetChunkVisibility(const Chunk *ChunkVal, FaceVisibility Vis) {
    int Slot = ChunkVisibilitySlot(ChunkVal);
    if (Slot >= 0) { GetChunkVisibilityTable()->Slots[Slot] = Vis; }
}

static void MarkColumnRunsStale(int ChunkX, int ChunkZ);

static void SetChunkOpaqueLayers(const Chunk *ChunkVal, OpaqueLayerMask Layers) {
    int Slot = ChunkVisibilitySlot(ChunkVal);
    if (Slot < 0) { return; }
    ChunkVisibilityTable *Table = GetChunkVisibilityTable();
    if (Table->Layers[Slot] == Layers) { return; }
    Table->Layers[Slot] = Layers;
    MarkColumnRunsStale(ChunkVal->ChunkX, ChunkVal->ChunkZ);
}

static OpaqueLayerMask GetChunkOpaqueLayers(const Chunk *ChunkVal) {
    int Slot = ChunkVisibilitySlot(ChunkVal);
    return (Slot < 0) ? OPAQUE_LAYERS_NONE : GetChunkVisibilityTable()->Layers[Slot];
}

#ifdef MINEGAME_PROFILE
void RendererLogVisibilityStats(const World *WorldVal) {
    const ChunkVisibilityTable *Table = GetChunkVisibilityTable();
    int Unknown = 0;
    int Sealed = 0;
    int Open = 0;
    int Partial = 0;
    for (int Idx = 0; Idx < WorldVal->ChunkCount; Idx++) {
        if (!WorldVal->SlotInUse[Idx]) { continue; }
        FaceVisibility Vis = Table->Slots[Idx];
        if (Vis == FACE_VIS_UNKNOWN) {
            Unknown++;
        } else if (Vis == FACE_VIS_NONE) {
            Sealed++;
        } else if (Vis == FACE_VIS_ALL) {
            Open++;
        } else {
            Partial++;
        }
    }
    LogInfo("RENDER: chunk visibility: %d open, %d partial, %d sealed, %d unknown",
            Open, Partial, Sealed, Unknown);
}
#endif

// ---------------------------------------------------------------------------
// Renderable set
// ---------------------------------------------------------------------------

// The chunks owning at least one mesh, grouped by column. Most loaded slots are
// air or buried rock with nothing to draw, and the chunks stacked in one column
// share their X/Z footprint, so the world draw tests a column's box once and
// only descends to its chunks when the box straddles the frustum.
//
// Records and columns are 1-based so that 0 means "none" everywhere: in
// Chunk.RenderRecord, in the column links and in the hash. A chunk owns a record
// only while it occupies a slot, and every column holds at least one record, so
// MAX_ACTIVE_CHUNKS bounds both pools. Columns can outnumber MAX_LOADED_COLUMNS
// while the load centre moves and old chunks wait for eviction, which is why
// the column pool is not sized by it.
enum {
    RENDER_COLUMN_HASH_SIZE = 1048576,
    RENDER_COLUMN_HASH_MASK = RENDER_COLUMN_HASH_SIZE - 1
};
_Static_assert(RENDER_COLUMN_HASH_SIZE >= 2 * MAX_ACTIVE_CHUNKS,
               "column hash must stay at or below half load");
_Static_assert((RENDER_COLUMN_HASH_SIZE & RENDER_COLUMN_HASH_MASK) == 0,
               "column hash size must be a power of two");

// Everything the per-frame sweep and the draw passes read about a chunk, so
// neither touches the Chunk itself: its fields sit on two cache lines past the
// block storage pointers, and the frame used to miss on both for every visible
// chunk, once in the sweep and again in the passes. Kept at 32 bytes, two per
// cache line: the chunk is named by its world slot rather than a pointer, its
// column by its coordinates, and a mesh is present exactly when its handle is
// valid.
typedef struct {
    int Slot;
    int PrevInColumn;
    int NextInColumn;
    MeshHandle Meshes[MESH_TARGET_COUNT];
    int16_t ChunkY;
    // What this chunk contributes to RenderableSet.VertexTotals, so an unload
    // subtracts exactly what the upload added; also what a draw submits.
    uint16_t VertexCounts[MESH_TARGET_COUNT];
} RenderableRecord;

enum { RENDER_RECORD_BYTES = 32 };
_Static_assert(sizeof(RenderableRecord) == RENDER_RECORD_BYTES,
               "a render record is half a cache line");
_Static_assert(MAX_MESH_VERTICES <= UINT16_MAX, "a mesh's vertex count fits 16 bits");

enum { COLUMN_OCCLUDER_RUNS = 2 };

typedef struct {
    int ChunkX;
    int ChunkZ;
    // Conservative: removals only mark the extent stale, so until it is
    // recomputed it can only be wider than the members, never narrower.
    int MinChunkY;
    int MaxChunkY;
    int Head;
    int Count;
    int DenseIndex;
    // Occluder runs for the horizon cull: the topmost stacks of fully opaque
    // layers found walking down from MaxChunkY, as inclusive world block rows,
    // topmost first. A chunk that is absent or not yet analysed ends a run, so
    // a run never spans anything unverified. Recomputed lazily: every layer
    // mask change in the column sets RunsStale.
    int16_t RunTop[COLUMN_OCCLUDER_RUNS];
    int16_t RunBottom[COLUMN_OCCLUDER_RUNS];
    uint8_t RunCount;
    bool RunsStale;
    bool ExtentStale;
    // The tile holding the column (0 when the tile pool was full) and the
    // column's neighbours in that tile's list.
    int Tile;
    int TilePrev;
    int TileNext;
} RenderColumn;

// Square groups of RENDER_TILE_SIDE x RENDER_TILE_SIDE columns. At the larger
// render distances most loaded columns lie wholly outside the view frustum,
// and testing their tile once settles all of them: at R=128 about 860 tile
// tests replace some 51,000 column tests, which also no longer touch the
// columns' cold memory. A tile's extent covers its columns' extents, which are
// themselves only ever too wide, so a tile classified outside or inside is so
// for every column in it.
//
// Tiles are 1-based like columns. The pool covers every loaded disc several
// times over; should it still run out, the column stays untiled and the sweep
// goes back to testing every column for as long as any is.
enum {
    RENDER_TILE_SIDE = 8,
    RENDER_TILE_CAPACITY = 16384,
    RENDER_TILE_HASH_SIZE = 32768,
    RENDER_TILE_HASH_MASK = RENDER_TILE_HASH_SIZE - 1
};
_Static_assert(RENDER_TILE_HASH_SIZE >= 2 * RENDER_TILE_CAPACITY,
               "tile hash must stay at or below half load");
_Static_assert((RENDER_TILE_HASH_SIZE & RENDER_TILE_HASH_MASK) == 0,
               "tile hash size must be a power of two");

typedef struct {
    int TileX;
    int TileZ;
    // Conservative like a column's: only ever too wide until refreshed.
    int MinChunkY;
    int MaxChunkY;
    int Head;
    int Count;
    int DenseIndex;
    bool ExtentStale;
} RenderTile;

typedef struct {
    RenderableRecord Records[MAX_ACTIVE_CHUNKS + 1];
    int FreeRecords[MAX_ACTIVE_CHUNKS];
    int FreeRecordCount;
    int RecordsEverUsed;
    int RecordCount;

    RenderColumn Columns[MAX_ACTIVE_CHUNKS + 1];
    int FreeColumns[MAX_ACTIVE_CHUNKS];
    int FreeColumnCount;
    int ColumnsEverUsed;
    int ColumnHash[RENDER_COLUMN_HASH_SIZE];

    // The live columns, packed, so the per-frame walk touches nothing else.
    int ActiveColumns[MAX_ACTIVE_CHUNKS];
    int ActiveColumnCount;

    RenderTile Tiles[RENDER_TILE_CAPACITY + 1];
    int FreeTiles[RENDER_TILE_CAPACITY];
    int FreeTileCount;
    int TilesEverUsed;
    int TileHash[RENDER_TILE_HASH_SIZE];
    int ActiveTiles[RENDER_TILE_CAPACITY];
    int ActiveTileCount;
    // Live columns no tile could take.
    int UntiledColumns;

    int64_t VertexTotals[MESH_TARGET_COUNT];
} RenderableSet;

static RenderableSet *GetRenderableSet(void) {
    static RenderableSet Set;
    return &Set;
}

// Spatial-hash primes; the final shift folds the high bits, where the products
// carry most of their entropy, into the masked low bits.
#define COLUMN_HASH_PRIME_X 73856093U
#define COLUMN_HASH_PRIME_Z 19349663U
#define COLUMN_HASH_FOLD_SHIFT 15U

static int ColumnHomeSlot(int ChunkX, int ChunkZ) {
    unsigned int Hash = ((unsigned int)ChunkX * COLUMN_HASH_PRIME_X) ^
                        ((unsigned int)ChunkZ * COLUMN_HASH_PRIME_Z);
    Hash ^= Hash >> COLUMN_HASH_FOLD_SHIFT;
    return (int)(Hash & (unsigned int)RENDER_COLUMN_HASH_MASK);
}

static int TileHomeSlot(int TileX, int TileZ) {
    unsigned int Hash = ((unsigned int)TileX * COLUMN_HASH_PRIME_X) ^
                        ((unsigned int)TileZ * COLUMN_HASH_PRIME_Z);
    Hash ^= Hash >> COLUMN_HASH_FOLD_SHIFT;
    return (int)(Hash & (unsigned int)RENDER_TILE_HASH_MASK);
}

// The tile at (TileX, TileZ), created empty if absent; 0 when the pool is full.
static int FindOrCreateTile(RenderableSet *Set, int TileX, int TileZ) {
    int Slot = TileHomeSlot(TileX, TileZ);
    while (Set->TileHash[Slot] != 0) {
        int Existing = Set->TileHash[Slot];
        if (Set->Tiles[Existing].TileX == TileX && Set->Tiles[Existing].TileZ == TileZ) {
            return Existing;
        }
        Slot = (Slot + 1) & RENDER_TILE_HASH_MASK;
    }
    if (Set->FreeTileCount == 0 && Set->TilesEverUsed == RENDER_TILE_CAPACITY) {
        return 0;
    }
    int Tile = (Set->FreeTileCount > 0) ? Set->FreeTiles[--Set->FreeTileCount]
                                        : ++Set->TilesEverUsed;
    Set->TileHash[Slot] = Tile;
    Set->Tiles[Tile] = (RenderTile){
        .TileX = TileX,
        .TileZ = TileZ,
        .MinChunkY = INT_MAX,
        .MaxChunkY = INT_MIN,
        .DenseIndex = Set->ActiveTileCount,
    };
    Set->ActiveTiles[Set->ActiveTileCount++] = Tile;
    return Tile;
}

// Backward-shift deletion, as for the column hash.
static void ReleaseTile(RenderableSet *Set, int Tile) {
    const RenderTile *Entry = &Set->Tiles[Tile];
    int Hole = TileHomeSlot(Entry->TileX, Entry->TileZ);
    while (Set->TileHash[Hole] != Tile) {
        Hole = (Hole + 1) & RENDER_TILE_HASH_MASK;
    }
    Set->TileHash[Hole] = 0;
    int Current = (Hole + 1) & RENDER_TILE_HASH_MASK;
    while (Set->TileHash[Current] != 0) {
        const RenderTile *Moving = &Set->Tiles[Set->TileHash[Current]];
        int Home = TileHomeSlot(Moving->TileX, Moving->TileZ);
        int FromHome = (Current - Home) & RENDER_TILE_HASH_MASK;
        int FromHole = (Current - Hole) & RENDER_TILE_HASH_MASK;
        if (FromHome >= FromHole) {
            Set->TileHash[Hole] = Set->TileHash[Current];
            Set->TileHash[Current] = 0;
            Hole = Current;
        }
        Current = (Current + 1) & RENDER_TILE_HASH_MASK;
    }

    int Dense = Entry->DenseIndex;
    int Last = --Set->ActiveTileCount;
    if (Dense != Last) {
        int Moved = Set->ActiveTiles[Last];
        Set->ActiveTiles[Dense] = Moved;
        Set->Tiles[Moved].DenseIndex = Dense;
    }
    Set->FreeTiles[Set->FreeTileCount++] = Tile;
}

static void AttachColumnToTile(RenderableSet *Set, int Column) {
    RenderColumn *Col = &Set->Columns[Column];
    int Tile = FindOrCreateTile(Set, FloorDivCells(Col->ChunkX, RENDER_TILE_SIDE),
                                FloorDivCells(Col->ChunkZ, RENDER_TILE_SIDE));
    Col->Tile = Tile;
    Col->TilePrev = 0;
    if (Tile == 0) {
        Col->TileNext = 0;
        Set->UntiledColumns++;
        return;
    }
    RenderTile *Entry = &Set->Tiles[Tile];
    Col->TileNext = Entry->Head;
    if (Entry->Head != 0) { Set->Columns[Entry->Head].TilePrev = Column; }
    Entry->Head = Column;
    Entry->Count++;
}

static void DetachColumnFromTile(RenderableSet *Set, int Column) {
    const RenderColumn *Col = &Set->Columns[Column];
    if (Col->Tile == 0) {
        Set->UntiledColumns--;
        return;
    }
    RenderTile *Entry = &Set->Tiles[Col->Tile];
    if (Col->TilePrev != 0) {
        Set->Columns[Col->TilePrev].TileNext = Col->TileNext;
    } else {
        Entry->Head = Col->TileNext;
    }
    if (Col->TileNext != 0) { Set->Columns[Col->TileNext].TilePrev = Col->TilePrev; }
    Entry->Count--;
    if (Entry->Count == 0) {
        ReleaseTile(Set, Col->Tile);
    } else {
        Entry->ExtentStale = true;
    }
}

// Grows the column's tile to cover the column's current extent.
static void WidenColumnTile(RenderableSet *Set, const RenderColumn *Col) {
    if (Col->Tile == 0) { return; }
    RenderTile *Entry = &Set->Tiles[Col->Tile];
    if (Col->MinChunkY < Entry->MinChunkY) { Entry->MinChunkY = Col->MinChunkY; }
    if (Col->MaxChunkY > Entry->MaxChunkY) { Entry->MaxChunkY = Col->MaxChunkY; }
}

static void MarkColumnTileStale(RenderableSet *Set, const RenderColumn *Col) {
    if (Col->Tile != 0) { Set->Tiles[Col->Tile].ExtentStale = true; }
}

static int FindOrCreateColumn(RenderableSet *Set, int ChunkX, int ChunkZ) {
    int Slot = ColumnHomeSlot(ChunkX, ChunkZ);
    while (Set->ColumnHash[Slot] != 0) {
        int Existing = Set->ColumnHash[Slot];
        if (Set->Columns[Existing].ChunkX == ChunkX &&
            Set->Columns[Existing].ChunkZ == ChunkZ) {
            return Existing;
        }
        Slot = (Slot + 1) & RENDER_COLUMN_HASH_MASK;
    }

    int Column = (Set->FreeColumnCount > 0)
                     ? Set->FreeColumns[--Set->FreeColumnCount]
                     : ++Set->ColumnsEverUsed;
    Set->ColumnHash[Slot] = Column;
    Set->Columns[Column] = (RenderColumn){
        .ChunkX = ChunkX,
        .ChunkZ = ChunkZ,
        .DenseIndex = Set->ActiveColumnCount,
        .RunsStale = true,
    };
    Set->ActiveColumns[Set->ActiveColumnCount++] = Column;
    AttachColumnToTile(Set, Column);
    return Column;
}

static int FindColumn(const RenderableSet *Set, int ChunkX, int ChunkZ) {
    int Slot = ColumnHomeSlot(ChunkX, ChunkZ);
    while (Set->ColumnHash[Slot] != 0) {
        int Existing = Set->ColumnHash[Slot];
        if (Set->Columns[Existing].ChunkX == ChunkX &&
            Set->Columns[Existing].ChunkZ == ChunkZ) {
            return Existing;
        }
        Slot = (Slot + 1) & RENDER_COLUMN_HASH_MASK;
    }
    return 0;
}

static void MarkColumnRunsStale(int ChunkX, int ChunkZ) {
    RenderableSet *Set = GetRenderableSet();
    int Column = FindColumn(Set, ChunkX, ChunkZ);
    if (Column != 0) { Set->Columns[Column].RunsStale = true; }
}

static void PushColumnRun(RenderColumn *Col, int Top, int Bottom) {
    Col->RunTop[Col->RunCount] = (int16_t)Top;
    Col->RunBottom[Col->RunCount] = (int16_t)Bottom;
    Col->RunCount++;
}

static void RefreshColumnRuns(World *WorldVal, RenderColumn *Col) {
    Col->RunCount = 0;
    Col->RunsStale = false;
    bool InRun = false;
    int Top = 0;
    int Bottom = 0;
    for (int ChunkY = Col->MaxChunkY; Col->RunCount < COLUMN_OCCLUDER_RUNS; ChunkY--) {
        const Chunk *ChunkVal = GetChunkFromWorld(WorldVal, Col->ChunkX, ChunkY, Col->ChunkZ);
        OpaqueLayerMask Layers =
            (ChunkVal != NULL) ? GetChunkOpaqueLayers(ChunkVal) : OPAQUE_LAYERS_NONE;
        for (int Layer = CHUNK_SIZE - 1;
             Layer >= 0 && Col->RunCount < COLUMN_OCCLUDER_RUNS; Layer--) {
            int Row = (ChunkY * CHUNK_SIZE) + Layer;
            if ((Layers & (1U << (unsigned)Layer)) != 0) {
                if (!InRun) { Top = Row; }
                InRun = true;
                Bottom = Row;
            } else if (InRun) {
                PushColumnRun(Col, Top, Bottom);
                InRun = false;
            }
        }
        if (ChunkVal == NULL) { break; }
    }
    if (InRun && Col->RunCount < COLUMN_OCCLUDER_RUNS) { PushColumnRun(Col, Top, Bottom); }
}

// Backward-shift deletion: after emptying the slot, every entry further along
// the probe run whose home does not lie cyclically in (Hole, Current] is moved
// back into the hole, so lookups never need tombstones.
static void RemoveColumnFromHash(RenderableSet *Set, int Column) {
    const RenderColumn *Col = &Set->Columns[Column];
    int Hole = ColumnHomeSlot(Col->ChunkX, Col->ChunkZ);
    while (Set->ColumnHash[Hole] != Column) {
        Hole = (Hole + 1) & RENDER_COLUMN_HASH_MASK;
    }
    Set->ColumnHash[Hole] = 0;

    int Current = (Hole + 1) & RENDER_COLUMN_HASH_MASK;
    while (Set->ColumnHash[Current] != 0) {
        const RenderColumn *Moving = &Set->Columns[Set->ColumnHash[Current]];
        int Home = ColumnHomeSlot(Moving->ChunkX, Moving->ChunkZ);
        int FromHome = (Current - Home) & RENDER_COLUMN_HASH_MASK;
        int FromHole = (Current - Hole) & RENDER_COLUMN_HASH_MASK;
        if (FromHome >= FromHole) {
            Set->ColumnHash[Hole] = Set->ColumnHash[Current];
            Set->ColumnHash[Current] = 0;
            Hole = Current;
        }
        Current = (Current + 1) & RENDER_COLUMN_HASH_MASK;
    }
}

static void ReleaseColumn(RenderableSet *Set, int Column) {
    DetachColumnFromTile(Set, Column);
    RemoveColumnFromHash(Set, Column);

    int Dense = Set->Columns[Column].DenseIndex;
    int Last = --Set->ActiveColumnCount;
    if (Dense != Last) {
        int Moved = Set->ActiveColumns[Last];
        Set->ActiveColumns[Dense] = Moved;
        Set->Columns[Moved].DenseIndex = Dense;
    }
    Set->FreeColumns[Set->FreeColumnCount++] = Column;
}

static void InsertRenderable(RenderableSet *Set, Chunk *ChunkVal) {
    int Record = (Set->FreeRecordCount > 0)
                     ? Set->FreeRecords[--Set->FreeRecordCount]
                     : ++Set->RecordsEverUsed;
    int Column = FindOrCreateColumn(Set, ChunkVal->ChunkX, ChunkVal->ChunkZ);
    RenderColumn *Col = &Set->Columns[Column];

    if (Col->Count == 0) {
        Col->MinChunkY = ChunkVal->ChunkY;
        Col->MaxChunkY = ChunkVal->ChunkY;
        Col->ExtentStale = false;
    } else {
        if (ChunkVal->ChunkY < Col->MinChunkY) { Col->MinChunkY = ChunkVal->ChunkY; }
        if (ChunkVal->ChunkY > Col->MaxChunkY) { Col->MaxChunkY = ChunkVal->ChunkY; }
    }
    WidenColumnTile(Set, Col);

    Set->Records[Record] = (RenderableRecord){
        .Slot = ChunkVisibilitySlot(ChunkVal),
        .NextInColumn = Col->Head,
        .Meshes = {ChunkVal->ChunkMesh, ChunkVal->CutoutMesh, ChunkVal->TranslucentMesh},
        .ChunkY = (int16_t)ChunkVal->ChunkY,
    };
    if (Col->Head != 0) { Set->Records[Col->Head].PrevInColumn = Record; }
    Col->Head = Record;
    Col->Count++;

    Set->RecordCount++;
    ChunkVal->RenderRecord = Record;
}

static void RemoveRenderable(RenderableSet *Set, Chunk *ChunkVal) {
    int Record = ChunkVal->RenderRecord;
    const RenderableRecord *Rec = &Set->Records[Record];
    int Column = FindColumn(Set, ChunkVal->ChunkX, ChunkVal->ChunkZ);
    RenderColumn *Col = &Set->Columns[Column];

    if (Rec->PrevInColumn != 0) {
        Set->Records[Rec->PrevInColumn].NextInColumn = Rec->NextInColumn;
    } else {
        Col->Head = Rec->NextInColumn;
    }
    if (Rec->NextInColumn != 0) {
        Set->Records[Rec->NextInColumn].PrevInColumn = Rec->PrevInColumn;
    }
    Col->Count--;

    if (Col->Count == 0) {
        ReleaseColumn(Set, Column);
    } else if (ChunkVal->ChunkY == Col->MinChunkY ||
               ChunkVal->ChunkY == Col->MaxChunkY) {
        Col->ExtentStale = true;
        MarkColumnTileStale(Set, Col);
    }

    Set->Records[Record] = (RenderableRecord){0};
    Set->FreeRecords[Set->FreeRecordCount++] = Record;
    Set->RecordCount--;
    ChunkVal->RenderRecord = 0;
}

static void SyncChunkRenderable(Chunk *ChunkVal) {
    bool Wants = ChunkVal->HasMesh || ChunkVal->HasCutoutMesh ||
                 ChunkVal->HasTranslucentMesh;
    bool Present = ChunkVal->RenderRecord != 0;
    if (Wants && !Present) {
        InsertRenderable(GetRenderableSet(), ChunkVal);
    } else if (!Wants && Present) {
        RemoveRenderable(GetRenderableSet(), ChunkVal);
    }
}

static void RefreshColumnExtent(const RenderableSet *Set, RenderColumn *Col) {
    Col->MinChunkY = Set->Records[Col->Head].ChunkY;
    Col->MaxChunkY = Col->MinChunkY;
    for (int Rec = Set->Records[Col->Head].NextInColumn; Rec != 0;
         Rec = Set->Records[Rec].NextInColumn) {
        int ChunkY = Set->Records[Rec].ChunkY;
        if (ChunkY < Col->MinChunkY) { Col->MinChunkY = ChunkY; }
        if (ChunkY > Col->MaxChunkY) { Col->MaxChunkY = ChunkY; }
    }
    Col->ExtentStale = false;
}

static void RefreshTileExtent(RenderableSet *Set, RenderTile *Entry) {
    Entry->MinChunkY = INT_MAX;
    Entry->MaxChunkY = INT_MIN;
    for (int Column = Entry->Head; Column != 0; Column = Set->Columns[Column].TileNext) {
        RenderColumn *Col = &Set->Columns[Column];
        if (Col->ExtentStale) { RefreshColumnExtent(Set, Col); }
        if (Col->MinChunkY < Entry->MinChunkY) { Entry->MinChunkY = Col->MinChunkY; }
        if (Col->MaxChunkY > Entry->MaxChunkY) { Entry->MaxChunkY = Col->MaxChunkY; }
    }
    Entry->ExtentStale = false;
}

// The backend frees every mesh at shutdown without going through
// UnloadChunkMesh, so the set is emptied with it. The chunks are not touched:
// the world has already been freed by the time the renderer closes.
static void ResetRenderableSet(void) {
    memset(GetRenderableSet(), 0, sizeof(RenderableSet));
}

void RendererGetOccluderRunStats(World *WorldVal, OccluderRunStats *Out) {
    RenderableSet *Set = GetRenderableSet();
    OccluderRunStats Stats = {.Columns = Set->ActiveColumnCount};
    for (int Dense = 0; Dense < Set->ActiveColumnCount; Dense++) {
        RenderColumn *Col = &Set->Columns[Set->ActiveColumns[Dense]];
        if (Col->ExtentStale) { RefreshColumnExtent(Set, Col); }
        if (Col->RunsStale) {
            RefreshColumnRuns(WorldVal, Col);
            Stats.Refreshed++;
        }
        if (Col->RunCount == 0) { continue; }
        Stats.ColumnsWithRun++;
        Stats.TopRunLayers += Col->RunTop[0] - Col->RunBottom[0] + 1;
        for (int Run = 0; Run < Col->RunCount; Run++) {
            Stats.Runs++;
            Stats.RunLayers += Col->RunTop[Run] - Col->RunBottom[Run] + 1;
        }
    }
    *Out = Stats;
}

#ifdef MINEGAME_PROFILE
static const Chunk *RecordChunk(const RenderableRecord *Rec) {
    return GetChunkVisibilityTable()->Base + Rec->Slot;
}

static bool CheckRenderableColumns(const RenderableSet *Set) {
    bool Consistent = true;
    int Members = 0;
    for (int Dense = 0; Dense < Set->ActiveColumnCount; Dense++) {
        int Column = Set->ActiveColumns[Dense];
        const RenderColumn *Col = &Set->Columns[Column];
        if (Col->DenseIndex != Dense) { Consistent = false; }

        int Length = 0;
        int Prev = 0;
        for (int Rec = Col->Head; Rec != 0; Rec = Set->Records[Rec].NextInColumn) {
            const RenderableRecord *Entry = &Set->Records[Rec];
            const Chunk *ChunkVal = RecordChunk(Entry);
            bool MeshesMatch = Entry->Meshes[MESH_TARGET_OPAQUE] == ChunkVal->ChunkMesh &&
                               Entry->Meshes[MESH_TARGET_CUTOUT] == ChunkVal->CutoutMesh &&
                               Entry->Meshes[MESH_TARGET_TRANSLUCENT] == ChunkVal->TranslucentMesh;
            if (Entry->PrevInColumn != Prev || !MeshesMatch ||
                ChunkVal->ChunkX != Col->ChunkX || ChunkVal->ChunkZ != Col->ChunkZ ||
                ChunkVal->ChunkY != Entry->ChunkY ||
                ChunkVal->ChunkY < Col->MinChunkY ||
                ChunkVal->ChunkY > Col->MaxChunkY) {
                Consistent = false;
            }
            Prev = Rec;
            Length++;
        }
        if (Length != Col->Count || Length == 0) { Consistent = false; }
        Members += Length;
    }

    int Hashed = 0;
    for (int Slot = 0; Slot < RENDER_COLUMN_HASH_SIZE; Slot++) {
        if (Set->ColumnHash[Slot] != 0) { Hashed++; }
    }

    int Tiled = 0;
    for (int Dense = 0; Dense < Set->ActiveTileCount; Dense++) {
        int Tile = Set->ActiveTiles[Dense];
        const RenderTile *Entry = &Set->Tiles[Tile];
        if (Entry->DenseIndex != Dense) { Consistent = false; }
        int Length = 0;
        int Prev = 0;
        for (int Column = Entry->Head; Column != 0; Column = Set->Columns[Column].TileNext) {
            const RenderColumn *Col = &Set->Columns[Column];
            if (Col->Tile != Tile || Col->TilePrev != Prev ||
                FloorDivCells(Col->ChunkX, RENDER_TILE_SIDE) != Entry->TileX ||
                FloorDivCells(Col->ChunkZ, RENDER_TILE_SIDE) != Entry->TileZ ||
                Col->MinChunkY < Entry->MinChunkY || Col->MaxChunkY > Entry->MaxChunkY) {
                Consistent = false;
            }
            Prev = Column;
            Length++;
        }
        if (Length != Entry->Count || Length == 0) { Consistent = false; }
        Tiled += Length;
    }
    return Consistent && Members == Set->RecordCount &&
           Hashed == Set->ActiveColumnCount &&
           Tiled + Set->UntiledColumns == Set->ActiveColumnCount;
}

bool RendererCheckRenderableSet(World *WorldVal) {
    const RenderableSet *Set = GetRenderableSet();
    int Expected = 0;
    bool Consistent = true;
    for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
        if (!WorldVal->SlotInUse[IdxI]) { continue; }
        const Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
        bool Wants = ChunkVal->HasMesh || ChunkVal->HasCutoutMesh ||
                     ChunkVal->HasTranslucentMesh;
        if (!Wants) { continue; }
        Expected++;
        int Record = ChunkVal->RenderRecord;
        if (Record <= 0 || Record > Set->RecordsEverUsed ||
            Set->Records[Record].Slot != IdxI) {
            Consistent = false;
        }
    }
    if (Expected != Set->RecordCount) { Consistent = false; }

    bool ColumnsConsistent = CheckRenderableColumns(Set);
    if (Consistent && ColumnsConsistent) {
        LogInfo("RENDER: renderable set consistent (%d chunks in %d columns)",
                Set->RecordCount, Set->ActiveColumnCount);
    } else {
        LogError("RENDER: renderable set inconsistent: %d records, %d chunks own "
                 "a mesh, columns %s",
                 Set->RecordCount, Expected, ColumnsConsistent ? "ok" : "corrupt");
    }
    return Consistent && ColumnsConsistent;
}
#endif

static void ReleaseRecordVertexCounts(Chunk *ChunkVal) {
    if (ChunkVal->RenderRecord == 0) { return; }
    RenderableSet *Set = GetRenderableSet();
    RenderableRecord *Rec = &Set->Records[ChunkVal->RenderRecord];
    for (int Target = 0; Target < MESH_TARGET_COUNT; Target++) {
        Set->VertexTotals[Target] -= Rec->VertexCounts[Target];
        Rec->VertexCounts[Target] = 0;
    }
}

static void RecordUploadedVertexCounts(const Chunk *ChunkVal,
                                       const MeshBufferSet Sets[MESH_TARGET_COUNT]) {
    if (ChunkVal->RenderRecord == 0) { return; }
    RenderableSet *Set = GetRenderableSet();
    RenderableRecord *Rec = &Set->Records[ChunkVal->RenderRecord];
    bool Uploaded[MESH_TARGET_COUNT] = {
        ChunkVal->HasMesh, ChunkVal->HasCutoutMesh, ChunkVal->HasTranslucentMesh};
    for (int Target = 0; Target < MESH_TARGET_COUNT; Target++) {
        int Count = Uploaded[Target] ? Sets[Target].VertexCount : 0;
        Rec->VertexCounts[Target] = (uint16_t)Count;
        Set->VertexTotals[Target] += Count;
    }
}

void RendererGetMeshVertexTotals(MeshVertexTotals *Out) {
    const RenderableSet *Set = GetRenderableSet();
    Out->Opaque = Set->VertexTotals[MESH_TARGET_OPAQUE];
    Out->Cutout = Set->VertexTotals[MESH_TARGET_CUTOUT];
    Out->Translucent = Set->VertexTotals[MESH_TARGET_TRANSLUCENT];
}

void UnloadChunkMesh(Chunk *ChunkVal) {
    // Unknown until a new result is committed. Every path that replaces a mesh,
    // evicts a slot or empties a chunk passes through here, so a slot never
    // keeps the connectivity of whatever occupied it before.
    SetChunkVisibility(ChunkVal, FACE_VIS_UNKNOWN);
    SetChunkOpaqueLayers(ChunkVal, OPAQUE_LAYERS_NONE);
    ReleaseRecordVertexCounts(ChunkVal);
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
    SyncChunkRenderable(ChunkVal);
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

int RendererCountFreeMeshJobs(void) {
    MeshJob *Jobs = GetMeshJobs();
    int Free = 0;
    for (int Idx = 0; Idx < MESH_JOB_SLOT_COUNT; Idx++) {
        if (atomic_load_explicit(&Jobs[Idx].Stage, memory_order_acquire) ==
            MESH_JOB_FREE) {
            Free++;
        }
    }
    return Free;
}

// Along the step axis, the neighbour's layer facing back toward the chunk;
// along the others, the free coordinate.
static int TouchingLayerCoord(int Step, int Free) {
    if (Step > 0) { return 0; }
    if (Step < 0) { return CHUNK_SIZE - 1; }
    return Free;
}

// Whether the layer of the neighbour at (Dx, Dy, Dz) that touches ChunkVal is
// opaque throughout, so the mesher would hide every face ChunkVal shows it. A
// neighbour not yet generated reads as air to the snapshot, so it never seals;
// an absent one the load left out as rock reads as opaque stone, so it does.
static bool NeighbourSideSealed(World *WorldVal, const Chunk *ChunkVal, int Dx,
                                int Dy, int Dz) {
    int X = ChunkVal->ChunkX + Dx;
    int Y = ChunkVal->ChunkY + Dy;
    int Z = ChunkVal->ChunkZ + Dz;
    const Chunk *Neighbour = GetChunkFromWorld(WorldVal, X, Y, Z);
    if (Neighbour == NULL) {
        return WorldIsUnloadedRock(WorldVal, X, Y, Z);
    }
    if (!Neighbour->IsGenerated) {
        return false;
    }
    if (Neighbour->IsUniform) {
        return IsBlockOpaqueForVisibility(Neighbour->UniformBlockId);
    }
    for (int IdxA = 0; IdxA < CHUNK_SIZE; IdxA++) {
        for (int IdxB = 0; IdxB < CHUNK_SIZE; IdxB++) {
            int X = TouchingLayerCoord(Dx, IdxA);
            int Y = TouchingLayerCoord(Dy, (Dx != 0) ? IdxA : IdxB);
            int Z = TouchingLayerCoord(Dz, IdxB);
            if (!IsBlockOpaqueForVisibility(ChunkGetLocal(Neighbour, X, Y, Z))) {
                return false;
            }
        }
    }
    return true;
}

// A uniform opaque chunk shows a face only toward a neighbour cell that is not
// opaque. With all six touching layers opaque its mesh is empty and it connects
// no two faces, which is exactly what a worker would have produced.
static bool IsSealedUniformSolid(World *WorldVal, const Chunk *ChunkVal) {
    return ChunkVal->IsUniform &&
           IsBlockOpaqueForVisibility(ChunkVal->UniformBlockId) &&
           NeighbourSideSealed(WorldVal, ChunkVal, -1, 0, 0) &&
           NeighbourSideSealed(WorldVal, ChunkVal, 1, 0, 0) &&
           NeighbourSideSealed(WorldVal, ChunkVal, 0, -1, 0) &&
           NeighbourSideSealed(WorldVal, ChunkVal, 0, 1, 0) &&
           NeighbourSideSealed(WorldVal, ChunkVal, 0, 0, -1) &&
           NeighbourSideSealed(WorldVal, ChunkVal, 0, 0, 1);
}

bool RendererResolveEmptyChunk(World *WorldVal, Chunk *ChunkVal) {
    // Unloaded rock beside an empty chunk still shows faces into it.
    if (ChunkVal->SolidBlockCount != 0 ||
        WorldUnloadedRockFaces(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY,
                               ChunkVal->ChunkZ) != 0U) {
        return false;
    }
    UnloadChunkMesh(ChunkVal);
    SetChunkVisibility(ChunkVal, FACE_VIS_ALL);
    ChunkVal->IsDirty = false;
    return true;
}

int RendererQueueMeshJob(World *WorldVal, Chunk *ChunkVal) {
    // An empty chunk needs no mesh at all, so it is resolved here rather than
    // occupying a slot and a worker to produce nothing.
    if (RendererResolveEmptyChunk(WorldVal, ChunkVal)) {
        return -1;
    }

    if (IsSealedUniformSolid(WorldVal, ChunkVal)) {
        UnloadChunkMesh(ChunkVal);
        SetChunkVisibility(ChunkVal, FACE_VIS_NONE);
        SetChunkOpaqueLayers(ChunkVal, OPAQUE_LAYERS_ALL);
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

    BuildCullColumns(Snap, &State->Cull);
    static const int FACE_DIRS_COUNT = (int)(sizeof(FACE_DIRS) / sizeof(FACE_DIRS[0]));
    for (int Fd = 0; Fd < FACE_DIRS_COUNT; Fd++) {
        BuildGreedyFacePass(Snap, &State->Cull, &FACE_DIRS[Fd], State);
    }
    BuildRockCapPass(Snap, &State->Cull, State);
    BuildCrossPass(Snap, State);

    PROFILE_END(MeshBuildStart, PROFILE_MESH_BUILD);

    PROFILE_BEGIN(VisibilityStart);
    Job->Visibility =
        ComputeFaceVisibility(Snap, &State->Visibility, &Job->OpaqueLayers);
    PROFILE_END(VisibilityStart, PROFILE_MESH_VISIBILITY);

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
    // Round robin from the slot after the last one uploaded. Scanning from slot
    // 0 every time starved the high slots: with more slots than the per-frame
    // upload ceiling, the low slots were refilled and ready again every frame,
    // and a mesh in a high slot could wait for the whole load to drain.
    static int NextSlot = 0;
    MeshJob *Jobs = GetMeshJobs();
    for (int Step = 0; Step < MESH_JOB_SLOT_COUNT; Step++) {
        int Idx = (NextSlot + Step) % MESH_JOB_SLOT_COUNT;
        MeshJob *Job = &Jobs[Idx];
        if (atomic_load_explicit(&Job->Stage, memory_order_acquire) !=
            MESH_JOB_READY) {
            continue;
        }
        NextSlot = (Idx + 1) % MESH_JOB_SLOT_COUNT;

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
                UploadMeshBufferSet(&Job->Sets[MESH_TARGET_OPAQUE], GetChunkWorldOrigin(ChunkVal));
            ChunkVal->HasMesh = (ChunkVal->ChunkMesh != MESH_HANDLE_INVALID);

            ChunkVal->CutoutMesh =
                UploadMeshBufferSet(&Job->Sets[MESH_TARGET_CUTOUT], GetChunkWorldOrigin(ChunkVal));
            ChunkVal->HasCutoutMesh =
                (ChunkVal->CutoutMesh != MESH_HANDLE_INVALID);

            ChunkVal->TranslucentMesh =
                UploadMeshBufferSet(&Job->Sets[MESH_TARGET_TRANSLUCENT], GetChunkWorldOrigin(ChunkVal));
            ChunkVal->HasTranslucentMesh =
                (ChunkVal->TranslucentMesh != MESH_HANDLE_INVALID);
            SyncChunkRenderable(ChunkVal);
            RecordUploadedVertexCounts(ChunkVal, Job->Sets);
            SetChunkVisibility(ChunkVal, Job->Visibility);
            SetChunkOpaqueLayers(ChunkVal, Job->OpaqueLayers);

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
static bool IsChunkCellVisible(const RenderFrustum *Frustum, int ChunkX, int ChunkY,
                               int ChunkZ) {
    float const HALFCHUNKSIZE = CHUNK_SIZE / 2.0F;
    float Cx = (float)(ChunkX * CHUNK_SIZE) + HALFCHUNKSIZE;
    float Cy = (float)(ChunkY * CHUNK_SIZE) + HALFCHUNKSIZE;
    float Cz = (float)(ChunkZ * CHUNK_SIZE) + HALFCHUNKSIZE;

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

static bool IsChunkVisible(const RenderFrustum *Frustum, const Chunk *ChunkVal) {
    return IsChunkCellVisible(Frustum, ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
}

typedef enum {
    BOX_OUTSIDE = 0,
    BOX_INSIDE,
    BOX_INTERSECTING
} BoxClass;

// Per plane, the box corner furthest along the normal (p-vertex) decides
// whether the box is wholly behind it, and the corner furthest against it
// (n-vertex) whether the box is wholly in front. Outside on any plane is final;
// inside requires every plane.
static BoxClass ClassifyColumnBox(const RenderFrustum *Frustum, Vec3 Min, Vec3 Max) {
    BoxClass Result = BOX_INSIDE;
    for (int P = 0; P < FRUSTUM_PLANE_COUNT; P++) {
        float Nx = Frustum->Planes[P][0];
        float Ny = Frustum->Planes[P][1];
        float Nz = Frustum->Planes[P][2];
        float D = Frustum->Planes[P][3];

        float Far = (Nx * ((Nx >= 0.0F) ? Max.x : Min.x)) +
                    (Ny * ((Ny >= 0.0F) ? Max.y : Min.y)) +
                    (Nz * ((Nz >= 0.0F) ? Max.z : Min.z)) + D;
        if (Far < 0.0F) { return BOX_OUTSIDE; }

        float Near = (Nx * ((Nx >= 0.0F) ? Min.x : Max.x)) +
                     (Ny * ((Ny >= 0.0F) ? Min.y : Max.y)) +
                     (Nz * ((Nz >= 0.0F) ? Min.z : Max.z)) + D;
        if (Near < 0.0F) { Result = BOX_INTERSECTING; }
    }
    return Result;
}

// A column's box spans its chunks' actual geometry: blocks are centred on
// integer coordinates, so chunk C covers [C*16 - 0.5, C*16 + 15.5].
static void GetColumnBox(const RenderColumn *Col, Vec3 *OutMin, Vec3 *OutMax) {
    *OutMin = (Vec3){
        (float)(Col->ChunkX * CHUNK_SIZE) - BLOCK_HALF_SIZE,
        (float)(Col->MinChunkY * CHUNK_SIZE) - BLOCK_HALF_SIZE,
        (float)(Col->ChunkZ * CHUNK_SIZE) - BLOCK_HALF_SIZE,
    };
    *OutMax = (Vec3){
        (float)((Col->ChunkX + 1) * CHUNK_SIZE) - BLOCK_HALF_SIZE,
        (float)((Col->MaxChunkY + 1) * CHUNK_SIZE) - BLOCK_HALF_SIZE,
        (float)((Col->ChunkZ + 1) * CHUNK_SIZE) - BLOCK_HALF_SIZE,
    };
}

static RenderCullStats *GetCullStatsState(void) {
    static RenderCullStats Stats;
    return &Stats;
}

int GetLastRenderedChunkCount(void) { return GetCullStatsState()->Rendered; }

void GetLastCullStats(RenderCullStats *Out) { *Out = *GetCullStatsState(); }

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
// The bucket count is the largest key the loaded set can produce. Loaded chunks
// lie in a cylinder around the load centre: dx^2 + dz^2 <= R^2, and with the
// centre inside the world every loaded layer, window or terrain band, keeps
// |dy| within MAX_VERTICAL_LOAD_REACH. The key therefore never exceeds
// MAX_RENDER_DISTANCE^2 + MAX_VERTICAL_LOAD_REACH^2 and every distance stays
// exact. The camera can sit away from the load centre (freecam, or a centre
// outside the world), and those keys clamp into the last bucket: the far chunks
// then draw in arbitrary order relative to each other, which is degradation
// rather than breakage.
enum {
    CHUNK_DISTANCE_BUCKETS = (MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE) +
                             (MAX_VERTICAL_LOAD_REACH * MAX_VERTICAL_LOAD_REACH) +
                             1
};
_Static_assert(MAX_VERTICAL_LOAD_REACH >= MAX_LOAD_LAYER_REACH,
               "distance buckets must cover the window too");

// A visible chunk, by render record, and its distance bucket.
typedef struct {
    int Record;
    int Bucket;
} ChunkOrderEntry;

// Tie order for scripted runs: a chunk's grid position relative to the
// camera's chunk, packed so that comparing keys compares X, then Y, then Z.
// Relative positions order exactly as absolute ones, and the packing holds
// them while every visible chunk lies within TIE_KEY_REACH_XZ columns and
// TIE_KEY_REACH_Y layers of the camera, many times the largest render
// distance; past that, positions clamp and may tie.
enum {
    TIE_KEY_BITS_XZ = 11,
    TIE_KEY_BITS_Y = 10,
    TIE_KEY_REACH_XZ = 1 << (TIE_KEY_BITS_XZ - 1),
    TIE_KEY_REACH_Y = 1 << (TIE_KEY_BITS_Y - 1)
};
_Static_assert((2 * TIE_KEY_BITS_XZ) + TIE_KEY_BITS_Y == (int)(sizeof(uint32_t) * CHAR_BIT),
               "a tie key fills its 32 bits");

static uint32_t TieKeyField(int Delta, int Reach) {
    if (Delta < -Reach) { Delta = -Reach; }
    if (Delta > Reach - 1) { Delta = Reach - 1; }
    return (uint32_t)(Delta + Reach);
}

static uint32_t ChunkTieKey(int DeltaX, int DeltaY, int DeltaZ) {
    return (TieKeyField(DeltaX, TIE_KEY_REACH_XZ) << (TIE_KEY_BITS_Y + TIE_KEY_BITS_XZ)) |
           (TieKeyField(DeltaY, TIE_KEY_REACH_Y) << TIE_KEY_BITS_XZ) |
           TieKeyField(DeltaZ, TIE_KEY_REACH_XZ);
}

// Insertion sort of each bucket's run of the counting-sorted order, by tie
// key, moving each record with its key. Ends holds every bucket's end offset,
// which is what the scatter left in the offsets. Keys are precomputed so the
// sort reads no chunk: it runs before any draw is submitted, so its time adds
// to the frame even when the GPU is the bottleneck.
static void SortDistanceTies(int *Order, uint32_t *Keys, const int *Ends, const int *Counts) {
    for (int B = 0; B < CHUNK_DISTANCE_BUCKETS; B++) {
        int Start = Ends[B] - Counts[B];
        for (int IdxI = Start + 1; IdxI < Ends[B]; IdxI++) {
            int Record = Order[IdxI];
            uint32_t Key = Keys[IdxI];
            int IdxJ = IdxI - 1;
            while (IdxJ >= Start && Key < Keys[IdxJ]) {
                Order[IdxJ + 1] = Order[IdxJ];
                Keys[IdxJ + 1] = Keys[IdxJ];
                IdxJ--;
            }
            Order[IdxJ + 1] = Record;
            Keys[IdxJ + 1] = Key;
        }
    }
}

static int ChunkDistanceBucket(int DeltaX, int DeltaY, int DeltaZ) {
    int Bucket = (DeltaX * DeltaX) + (DeltaY * DeltaY) + (DeltaZ * DeltaZ);
    if (Bucket >= CHUNK_DISTANCE_BUCKETS) { Bucket = CHUNK_DISTANCE_BUCKETS - 1; }
    return Bucket;
}


// ---------------------------------------------------------------------------
// Occlusion traversal
// ---------------------------------------------------------------------------

// Walks the loaded chunk grid outward from the camera and stamps every chunk a
// sight line could reach. A step leaves a chunk through face F only when:
//   - it does not reverse a direction already taken from the camera's chunk (a
//     straight sight line is monotonic on every axis);
//   - the chunk connects the face it was entered by to F;
//   - the neighbour is loaded and touches the view frustum.
//
// Every step moves one chunk further from the camera's chunk in Manhattan
// distance, since none may reverse an axis already travelled. With a FIFO queue,
// every arrival at a chunk therefore comes from the previous distance and lands
// before the chunk itself is dequeued. A chunk is expanded once, with the union
// of the faces it was entered by and the intersection of the directions its
// arrivals took -- only ever more permissive than any single arrival, so
// nothing a sight line could reach is dropped (keeping just the first arrival
// would, and that is what makes chunks pop in and out at some angles). Should
// an arrival still widen a chunk after its expansion, which only the many seeds
// of the boundary-layer start can cause, it is queued again; both fields change
// monotonically, so that settles.
enum {
    OCCLUSION_DIR_MASK = (1 << CHUNK_FACE_COUNT) - 1,
    // Entry flag of the camera's own chunk: the camera stands inside it, so its
    // connectivity does not limit where sight lines leave it.
    OCCLUSION_START_ENTRY = 1 << CHUNK_FACE_COUNT,
    // A slot sits in the queue at most once at a time.
    OCCLUSION_QUEUE_CAPACITY = MAX_ACTIVE_CHUNKS,
    // Index of the near plane in RenderFrustum (left, right, bottom, top, near,
    // far). The traversal ignores it: sight lines start at the camera, behind
    // the near plane, and a chunk holding only that first stretch of a line
    // must still pass.
    FRUSTUM_NEAR_PLANE = 4
};

typedef struct {
    // Slot + 1, 0 when never resolved.
    int Slot;
    uint32_t Epoch;
} OcclusionLink;

// Everything the traversal reads or writes per slot, packed so that visiting a
// chunk touches one cache line rather than one per field: slots are reached in
// no particular memory order, so separate arrays each cost their own miss.
typedef struct {
    uint32_t VisitStamp;
    // Valid while VisitStamp matches the frame.
    uint8_t EntryFaces;
    uint8_t Dirs;
    bool Queued;
    // Per-frame frustum result: a chunk reached through several faces is
    // tested once.
    bool FrustumInside;
    uint32_t FrustumStamp;
    // Bumped by eviction; a link made to an earlier epoch no longer holds.
    uint32_t Epoch;
    // Grid position, carried along the traversal so it never reads the Chunk:
    // its coordinates sit past the 8 KB block array, a cold line per neighbour.
    int Pos[3];
} OcclusionSlot;

typedef struct {
    uint32_t Frame;
    OcclusionSlot Slots[MAX_ACTIVE_CHUNKS];

    // Neighbour links kept across frames. A link names the neighbour's slot and
    // that slot's epoch when the link was made; eviction bumps the epoch, so a
    // link to a recycled slot misses and is resolved again through the chunk
    // hash, and clears the evicted slot's own links.
    OcclusionLink Neighbor[MAX_ACTIVE_CHUNKS][CHUNK_FACE_COUNT];

    int Queue[OCCLUSION_QUEUE_CAPACITY];
    int Head;
    int Count;
    int Reachable;
    int Nodes;
    // A sight line left the loaded chunks into air the load did not keep
    // (above a column's highest loaded layer). From the open sky nearly every
    // loaded surface is in view, so tracing on would cost far more than the
    // little it could still cull: the frame falls back to frustum culling.
    bool TouchedSky;
    // The fallback was forced by where the camera stands -- inside an opaque
    // block or in rock the load left out -- rather than by the open sky. From
    // there the camera sees out through culled back faces, so no opaque layer
    // hides anything and the horizon cull must stand down too.
    bool CameraEnclosed;
} OcclusionState;

static OcclusionState *GetOcclusionState(void) {
    static OcclusionState State;
    return &State;
}

static const int FACE_STEP[CHUNK_FACE_COUNT][3] = {
    {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
};

enum { TRAVERSAL_PLANE_COUNT = FRUSTUM_PLANE_COUNT - 1 };

// The frustum minus its near plane, rewritten once per frame for grid
// coordinates. For each plane the corner of a chunk box furthest along the
// normal is fixed by the normal's signs, so "the box lies wholly behind the
// plane" becomes A*X + B*Y + C*Z + K < 0 on the chunk's integer coordinates:
// three multiply-adds with no per-axis selection.
typedef struct {
    float A[TRAVERSAL_PLANE_COUNT];
    float B[TRAVERSAL_PLANE_COUNT];
    float C[TRAVERSAL_PLANE_COUNT];
    float K[TRAVERSAL_PLANE_COUNT];
} TraversalFrustum;

static void BuildTraversalFrustum(const RenderFrustum *Frustum,
                                  TraversalFrustum *Out) {
    int Index = 0;
    for (int P = 0; P < FRUSTUM_PLANE_COUNT; P++) {
        if (P == FRUSTUM_NEAR_PLANE) { continue; }
        float Nx = Frustum->Planes[P][0];
        float Ny = Frustum->Planes[P][1];
        float Nz = Frustum->Planes[P][2];
        // Box min corner is 16 * Chunk - 0.5 on each axis; the far corner adds
        // the chunk width on every axis whose normal component is positive.
        float FarOffset = (fmaxf(Nx, 0.0F) + fmaxf(Ny, 0.0F) + fmaxf(Nz, 0.0F)) *
                          (float)CHUNK_SIZE;
        Out->A[Index] = Nx * (float)CHUNK_SIZE;
        Out->B[Index] = Ny * (float)CHUNK_SIZE;
        Out->C[Index] = Nz * (float)CHUNK_SIZE;
        Out->K[Index] = Frustum->Planes[P][3] + FarOffset -
                        ((Nx + Ny + Nz) * BLOCK_HALF_SIZE);
        Index++;
    }
}

// Conservative box test against the frustum minus its near plane: rejects only
// a chunk wholly behind one of the other five planes. Takes grid coordinates so
// a position can be rejected before anything is looked up for it.
static bool IsChunkBoxInTraversalFrustum(const TraversalFrustum *Frustum,
                                         int ChunkX, int ChunkY, int ChunkZ) {
    float X = (float)ChunkX;
    float Y = (float)ChunkY;
    float Z = (float)ChunkZ;
    for (int P = 0; P < TRAVERSAL_PLANE_COUNT; P++) {
        float Far = (Frustum->A[P] * X) + (Frustum->B[P] * Y) +
                    (Frustum->C[P] * Z) + Frustum->K[P];
        if (Far < 0.0F) { return false; }
    }
    return true;
}

static void PushSlot(OcclusionState *State, int Slot) {
    int Tail = State->Head + State->Count;
    if (Tail >= OCCLUSION_QUEUE_CAPACITY) { Tail -= OCCLUSION_QUEUE_CAPACITY; }
    State->Queue[Tail] = Slot;
    State->Count++;
    State->Slots[Slot].Queued = true;
}

// EntryFaces holds one bit per face entered by, or OCCLUSION_START_ENTRY.
// Pos is the slot's grid position.
static void ArriveAt(OcclusionState *State, int Slot, const int Pos[3],
                     unsigned EntryFaces, unsigned Dirs) {
    if (State->Slots[Slot].VisitStamp != State->Frame) {
        State->Slots[Slot].VisitStamp = State->Frame;
        State->Slots[Slot].Pos[0] = Pos[0];
        State->Slots[Slot].Pos[1] = Pos[1];
        State->Slots[Slot].Pos[2] = Pos[2];
        State->Slots[Slot].EntryFaces = (uint8_t)EntryFaces;
        State->Slots[Slot].Dirs = (uint8_t)Dirs;
        State->Reachable++;
        PushSlot(State, Slot);
        return;
    }
    unsigned Faces = State->Slots[Slot].EntryFaces | EntryFaces;
    unsigned Merged = State->Slots[Slot].Dirs & Dirs;
    if (Faces == State->Slots[Slot].EntryFaces && Merged == State->Slots[Slot].Dirs) { return; }
    State->Slots[Slot].EntryFaces = (uint8_t)Faces;
    State->Slots[Slot].Dirs = (uint8_t)Merged;
    if (!State->Slots[Slot].Queued) { PushSlot(State, Slot); }
}

// Faces a chunk can be left through, given the faces it was entered by.
static unsigned ExitFaces(FaceVisibility Vis, unsigned EntryFaces) {
    if ((EntryFaces & (unsigned)OCCLUSION_START_ENTRY) != 0) {
        return OCCLUSION_DIR_MASK;
    }
    return FaceVisExits(Vis, EntryFaces);
}

// The slot of the loaded neighbour across Face if it touches the frustum, or -1;
// writes the neighbour's grid position to OutPos. A link whose epoch still
// matches is followed without touching the chunk hash or the chunk; otherwise
// the position is frustum-tested first, so the hash is probed only for
// neighbours that could matter.
static int ResolveNeighbor(World *WorldVal, const TraversalFrustum *Frustum,
                           OcclusionState *State, int Slot, int Face,
                           int OutPos[3]) {
    int X = State->Slots[Slot].Pos[0] + FACE_STEP[Face][0];
    int Y = State->Slots[Slot].Pos[1] + FACE_STEP[Face][1];
    int Z = State->Slots[Slot].Pos[2] + FACE_STEP[Face][2];
    OutPos[0] = X;
    OutPos[1] = Y;
    OutPos[2] = Z;

    OcclusionLink *Link = &State->Neighbor[Slot][Face];
    int Next = Link->Slot - 1;
    if (Next >= 0 && State->Slots[Next].Epoch == Link->Epoch) {
        if (State->Slots[Next].FrustumStamp != State->Frame) {
            State->Slots[Next].FrustumStamp = State->Frame;
            State->Slots[Next].FrustumInside = IsChunkBoxInTraversalFrustum(Frustum, X, Y, Z);
        }
        return State->Slots[Next].FrustumInside ? Next : -1;
    }

    if (!IsChunkBoxInTraversalFrustum(Frustum, X, Y, Z)) { return -1; }
    const Chunk *Found = GetChunkFromWorld(WorldVal, X, Y, Z);
    if (Found == NULL) {
        if (!State->TouchedSky && WorldIsUnloadedSky(WorldVal, X, Y, Z)) {
            State->TouchedSky = true;
        }
        return -1;
    }
    Next = (int)(Found - WorldVal->Chunks);
    *Link = (OcclusionLink){Next + 1, State->Slots[Next].Epoch};
    State->Slots[Next].FrustumStamp = State->Frame;
    State->Slots[Next].FrustumInside = true;
    return Next;
}

void RendererForgetChunk(Chunk *ChunkVal) {
    UnloadChunkMesh(ChunkVal);
    int Slot = ChunkVisibilitySlot(ChunkVal);
    if (Slot < 0) { return; }
    OcclusionState *State = GetOcclusionState();
    State->Slots[Slot].Epoch++;
    memset(State->Neighbor[Slot], 0, sizeof(State->Neighbor[Slot]));
}

static void ExpandChunk(World *WorldVal, const TraversalFrustum *Frustum,
                        OcclusionState *State, int Slot) {
    unsigned Dirs = State->Slots[Slot].Dirs;
    unsigned Exits = ExitFaces(GetChunkVisibilityTable()->Slots[Slot],
                               State->Slots[Slot].EntryFaces);
    for (int Face = 0; Face < CHUNK_FACE_COUNT; Face++) {
        if ((Exits & (1U << (unsigned)Face)) == 0) { continue; }
        int Back = (int)OppositeChunkFace((ChunkFace)Face);
        if ((Dirs & (1U << (unsigned)Back)) != 0) { continue; }
        int Pos[3];
        int Next = ResolveNeighbor(WorldVal, Frustum, State, Slot, Face, Pos);
        if (Next < 0) { continue; }
        ArriveAt(State, Next, Pos, 1U << (unsigned)Back,
                 Dirs | (1U << (unsigned)Face));
    }
}

static void BeginOcclusionFrame(OcclusionState *State) {
    State->Frame++;
    if (State->Frame == 0) {
        // Only the stamps: clearing epochs could revive links to recycled slots.
        for (int Slot = 0; Slot < MAX_ACTIVE_CHUNKS; Slot++) {
            State->Slots[Slot].VisitStamp = 0;
            State->Slots[Slot].FrustumStamp = 0;
        }
        State->Frame = 1;
    }
    State->Head = 0;
    State->Count = 0;
    State->Reachable = 0;
    State->Nodes = 0;
    State->TouchedSky = false;
    State->CameraEnclosed = false;
}

// Half-width of the box around the camera checked for opaque blocks. Covers
// the near-plane rectangle with a wide margin, since geometry the near plane
// clips away is just as see-through as geometry the camera sits inside.
#define CAMERA_CLEARANCE 0.1F
enum { CAMERA_PROBE_CORNERS = 8 };

// From inside an opaque block the camera looks out through back faces, which
// are culled, so the rock around it hides nothing and the face connectivity --
// which assumes the viewer stands in open space -- does not apply.
static bool IsCameraInsideOpaque(World *WorldVal, Vec3 CameraPos) {
    for (int Corner = 0; Corner < CAMERA_PROBE_CORNERS; Corner++) {
        float Ox = ((Corner & 1) != 0) ? CAMERA_CLEARANCE : -CAMERA_CLEARANCE;
        float Oy = ((Corner & 2) != 0) ? CAMERA_CLEARANCE : -CAMERA_CLEARANCE;
        float Oz = ((Corner & 4) != 0) ? CAMERA_CLEARANCE : -CAMERA_CLEARANCE;
        // Block B is drawn over [B - 0.5, B + 0.5], so rounding names it.
        Vec3 Block = {floorf(CameraPos.x + Ox + BLOCK_HALF_SIZE),
                      floorf(CameraPos.y + Oy + BLOCK_HALF_SIZE),
                      floorf(CameraPos.z + Oz + BLOCK_HALF_SIZE)};
        int Id = GetBlockIDFromWorld(WorldVal, Block);
        if (Id > 0 && IsBlockOpaqueForVisibility((BlockID)Id)) { return true; }
    }
    return false;
}

// Returns false when the frame must fall back to frustum culling alone.
static bool RunOcclusionTraversal(World *WorldVal, const RenderFrustum *Frustum,
                                  Vec3 CameraPos) {
    OcclusionState *State = GetOcclusionState();
    BeginOcclusionFrame(State);
    TraversalFrustum Planes;
    BuildTraversalFrustum(Frustum, &Planes);
    if (IsCameraInsideOpaque(WorldVal, CameraPos)) {
        State->CameraEnclosed = true;
        return false;
    }

    // Blocks are centred on integer coordinates, so chunk C spans
    // [16C - 0.5, 16C + 15.5]; the half-block shift picks the chunk the camera
    // is really in, not the one a plain floor names near a boundary.
    int CameraX = (int)floorf((CameraPos.x + BLOCK_HALF_SIZE) / CHUNK_SIZE);
    int CameraY = (int)floorf((CameraPos.y + BLOCK_HALF_SIZE) / CHUNK_SIZE);
    int CameraZ = (int)floorf((CameraPos.z + BLOCK_HALF_SIZE) / CHUNK_SIZE);

    const Chunk *Start = GetChunkFromWorld(WorldVal, CameraX, CameraY, CameraZ);
    if (Start != NULL) {
        int Pos[3] = {CameraX, CameraY, CameraZ};
        ArriveAt(State, (int)(Start - WorldVal->Chunks), Pos,
                 OCCLUSION_START_ENTRY, 0);
    } else {
        // In the open sky above its column, in rock left unloaded under a band,
        // or over a column holding nothing: no loaded chunk to start from that
        // is known to see what the camera sees.
        State->CameraEnclosed = WorldIsUnloadedRock(WorldVal, CameraX, CameraY, CameraZ);
        return false;
    }

    while (State->Count > 0) {
        if (State->TouchedSky) { return false; }

        int Slot = State->Queue[State->Head];
        State->Head++;
        if (State->Head == OCCLUSION_QUEUE_CAPACITY) { State->Head = 0; }
        State->Count--;
        State->Nodes++;

        State->Slots[Slot].Queued = false;
        ExpandChunk(WorldVal, &Planes, State, Slot);
    }
    return !State->TouchedSky;
}

static bool WasSlotReached(int Slot) {
    const OcclusionState *State = GetOcclusionState();
    return State->Slots[Slot].VisitStamp == State->Frame;
}

// ---------------------------------------------------------------------------
// Horizon culling
// ---------------------------------------------------------------------------
//
// Runs on the frames the occlusion traversal gives up on because sight lines
// reach the open sky, which is every frame at the surface. Columns are swept
// near to far; each one is first tested against a horizon built from the
// columns in front of it, then its fully opaque layer runs are added to it.
//
// A sight line is parameterised by camera-relative horizontal coordinates: with
// FwdXZ and RightXZ the camera's heading and its perpendicular in the XZ plane,
// a line through (dx, dy, dz) has fz = dot(dXZ, FwdXZ) > 0, u = dot(dXZ,
// RightXZ) / fz and v = dy / fz. u picks the vertical half-plane the line lies
// in and v is the tangent of its elevation, both independent of the pitch, and
// along a line height grows linearly with fz.
//
// Per u bin the horizon holds what is known to be blocked: every v below
// Prefix, and one floating interval [FloatLo, FloatHi]. The prefix starts at
// the bottom frustum plane, since lines below it are off screen. The floating
// interval exists because caves under the surface keep layer runs short (about
// 14 blocks on average), so the runs of the nearest columns rarely reach below
// the screen; the runs of successive columns still overlap one another and
// chain into a band that hides what lies wholly inside it.
//
// Every step only ever claims less than what is really blocked:
//   - A run spans whole 16x16 layers, so a line passing through the column at
//     a height inside the run meets an opaque block, whose face toward the
//     camera is drawn (the cell before it is open, or the line would have
//     stopped there). Its v range takes the bottom at the largest and the top
//     at the smallest v over the column's corners, so every point of the line's
//     chord through the column lies inside the run, and it is entered only into
//     bins wholly inside the column's inner u range (second smallest to second
//     largest corner u), where every chord has length.
//   - An occluder must lie in front of what it hides along every shared line.
//     Bucket keys are squared chunk distances from the camera's chunk, and a
//     column only joins the horizon once the sweep reaches keys of at least
//     (r + HORIZON_COMMIT_LAG)^2, r being its own key's root. Three chunks
//     cover both footprints' half-diagonals plus the camera's offset inside
//     its chunk (2.92 chunks), so every point of the occluder is nearer than
//     every point of anything it is then tested against.
//   - Columns with a corner at fz <= HORIZON_MIN_FZ, or reaching past the
//     frustum's u range, are never culled; occluders need every corner at
//     fz >= one chunk, which excludes the camera's column and anything the
//     near plane could clip open.
enum {
    HORIZON_BINS = 1024,
    HORIZON_COMMIT_LAG = 3,
    // Keys past the last bucket clamp into it: they then never commit as
    // occluders (their threshold exceeds every key) and are tested only
    // against occluders with real, smaller keys, which stays conservative.
    HORIZON_KEY_BUCKETS = (MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE) + 1,
    HORIZON_FRUSTUM_CORNERS = 4,
    FRUSTUM_LEFT_PLANE = 0,
    FRUSTUM_RIGHT_PLANE = 1,
    FRUSTUM_BOTTOM_PLANE = 2,
    FRUSTUM_TOP_PLANE = 3
};

// Slack on every v comparison, against float rounding in the projection.
#define HORIZON_SLOPE_EPS 1.0e-4F
// Smallest fz, in blocks, a tested column corner may have.
#define HORIZON_MIN_FZ 1.0e-2F
// Smallest heading component of a normalised frustum corner direction; past
// it (pitch beyond roughly 45 degrees at a 90-degree FovY) u is unbounded.
#define HORIZON_CORNER_MIN_FZ 5.0e-2F
#define HORIZON_MIN_HEADING 1.0e-3F
#define HORIZON_MIN_PLANE_UP 1.0e-4F

// Min, max, floor and ceil for the horizon's hot loops. Without SSE4.1 and
// with math errno semantics, fminf, fmaxf, floorf and ceilf compile to library
// calls, which cost more than the comparisons around them; these compile to
// minss, maxss and integer conversions. No value here is ever NaN, where the
// two would differ.
static inline float HorizonMin(float A, float B) { return (A < B) ? A : B; }

static inline float HorizonMax(float A, float B) { return (A > B) ? A : B; }

// For X well inside the int range. Truncation rounds toward zero, so a
// negative non-integer X truncates up and is stepped back down.
static inline int HorizonFloor(float X) {
    int I = (int)X;
    return ((float)I > X) ? I - 1 : I;
}

static inline int HorizonCeil(float X) {
    int I = (int)X;
    return ((float)I < X) ? I + 1 : I;
}

// One bin: every v below Prefix is blocked, and every v in [FloatLo, FloatHi]
// when FloatLo <= FloatHi. Interleaved so a span of bins reads consecutive
// memory once rather than three arrays.
typedef struct {
    float Prefix;
    float FloatLo;
    float FloatHi;
} HorizonBin;

typedef struct {
    float CamX;
    float CamY;
    float CamZ;
    float Fx;
    float Fz;
    float Rx;
    float Rz;
    float Umin;
    float Umax;
    float InvBinWidth;
    // Bounds over every bin and over the whole sweep so far: the largest
    // prefix, and the widest the floating bands have reached. A range they
    // cannot cover is visible in some bin, so it is kept without reading any.
    float PrefixMax;
    float FloatLoMin;
    float FloatHiMax;
    HorizonBin Bins[HORIZON_BINS];
} HorizonFrame;

static HorizonFrame *GetHorizonFrame(void) {
    static HorizonFrame Frame;
    return &Frame;
}

// Sets the frame up; false when the projection degenerates (camera looking
// nearly straight up or down) and the frame must go without the cull.
static bool BeginHorizonFrame(HorizonFrame *H, const RenderFrustum *Frustum,
                              RenderCamera Camera) {
    float Dx = Camera.Target.x - Camera.Position.x;
    float Dy = Camera.Target.y - Camera.Position.y;
    float Dz = Camera.Target.z - Camera.Position.z;
    float Heading = sqrtf((Dx * Dx) + (Dz * Dz));
    if (Heading < HORIZON_MIN_HEADING * sqrtf((Dx * Dx) + (Dy * Dy) + (Dz * Dz))) {
        return false;
    }
    H->CamX = Camera.Position.x;
    H->CamY = Camera.Position.y;
    H->CamZ = Camera.Position.z;
    H->Fx = Dx / Heading;
    H->Fz = Dz / Heading;
    H->Rx = -H->Fz;
    H->Rz = H->Fx;

    // Corner directions of the view, each where two side planes meet; the
    // visible region in (u, v) is the quadrilateral they span.
    static const int CORNER_PLANES[HORIZON_FRUSTUM_CORNERS][2] = {
        {FRUSTUM_LEFT_PLANE, FRUSTUM_BOTTOM_PLANE},
        {FRUSTUM_LEFT_PLANE, FRUSTUM_TOP_PLANE},
        {FRUSTUM_RIGHT_PLANE, FRUSTUM_BOTTOM_PLANE},
        {FRUSTUM_RIGHT_PLANE, FRUSTUM_TOP_PLANE},
    };
    H->Umin = HUGE_VALF;
    H->Umax = -HUGE_VALF;
    for (int Corner = 0; Corner < HORIZON_FRUSTUM_CORNERS; Corner++) {
        const float *A = Frustum->Planes[CORNER_PLANES[Corner][0]];
        const float *B = Frustum->Planes[CORNER_PLANES[Corner][1]];
        float Cx = (A[1] * B[2]) - (A[2] * B[1]);
        float Cy = (A[2] * B[0]) - (A[0] * B[2]);
        float Cz = (A[0] * B[1]) - (A[1] * B[0]);
        if ((Cx * Dx) + (Cy * Dy) + (Cz * Dz) < 0.0F) {
            Cx = -Cx;
            Cy = -Cy;
            Cz = -Cz;
        }
        float Len = sqrtf((Cx * Cx) + (Cy * Cy) + (Cz * Cz));
        float Fz = (Cx * H->Fx) + (Cz * H->Fz);
        if (Len <= 0.0F || Fz < HORIZON_CORNER_MIN_FZ * Len) { return false; }
        float U = ((Cx * H->Rx) + (Cz * H->Rz)) / Fz;
        H->Umin = HorizonMin(H->Umin, U);
        H->Umax = HorizonMax(H->Umax, U);
    }
    if (!(H->Umax > H->Umin)) { return false; }
    H->InvBinWidth = (float)HORIZON_BINS / (H->Umax - H->Umin);

    // A line is on screen only on the inner side of the bottom plane:
    // u * (N.R) + v * N.y + N.F >= 0, a line in (u, v). Over a bin it is lowest
    // at one of the bin's two ends.
    const float *Bottom = Frustum->Planes[FRUSTUM_BOTTOM_PLANE];
    float NormalUp = Bottom[1];
    if (NormalUp < HORIZON_MIN_PLANE_UP) { return false; }
    float NormalRight = (Bottom[0] * H->Rx) + (Bottom[2] * H->Rz);
    float NormalFwd = (Bottom[0] * H->Fx) + (Bottom[2] * H->Fz);
    float BinWidth = (H->Umax - H->Umin) / (float)HORIZON_BINS;
    H->PrefixMax = -HUGE_VALF;
    for (int Bin = 0; Bin < HORIZON_BINS; Bin++) {
        float U0 = H->Umin + ((float)Bin * BinWidth);
        float U1 = U0 + BinWidth;
        float V0 = -((U0 * NormalRight) + NormalFwd) / NormalUp;
        float V1 = -((U1 * NormalRight) + NormalFwd) / NormalUp;
        HorizonBin *Entry = &H->Bins[Bin];
        Entry->Prefix = HorizonMin(V0, V1) - HORIZON_SLOPE_EPS;
        Entry->FloatLo = HUGE_VALF;
        Entry->FloatHi = -HUGE_VALF;
        H->PrefixMax = HorizonMax(H->PrefixMax, Entry->Prefix);
    }
    H->FloatLoMin = HUGE_VALF;
    H->FloatHiMax = -HUGE_VALF;
    return true;
}

// A column's corners seen from the camera: fz extremes, the u range every
// corner spans (Outer) and the one between the second smallest and second
// largest corner u (Inner).
typedef struct {
    float FzMin;
    float FzMax;
    float InvFzMin;
    float InvFzMax;
    float OuterMin;
    float OuterMax;
    float InnerMin;
    float InnerMax;
} HorizonFootprint;

enum { FOOTPRINT_CORNERS = 4, FOOTPRINT_SORT_STEPS = 5 };

static bool ProjectFootprint(const HorizonFrame *H, int ChunkX, int ChunkZ,
                             HorizonFootprint *Out) {
    float X0 = ((float)(ChunkX * CHUNK_SIZE) - BLOCK_HALF_SIZE) - H->CamX;
    float Z0 = ((float)(ChunkZ * CHUNK_SIZE) - BLOCK_HALF_SIZE) - H->CamZ;
    // Both projections are linear, so the far corners are the near one plus
    // a chunk's step along X, Z or both.
    float Fz0 = (X0 * H->Fx) + (Z0 * H->Fz);
    float R0 = (X0 * H->Rx) + (Z0 * H->Rz);
    float StepFx = (float)CHUNK_SIZE * H->Fx;
    float StepFz = (float)CHUNK_SIZE * H->Fz;
    float StepRx = (float)CHUNK_SIZE * H->Rx;
    float StepRz = (float)CHUNK_SIZE * H->Rz;
    float Fz[FOOTPRINT_CORNERS] = {Fz0, Fz0 + StepFx, Fz0 + StepFz, Fz0 + StepFx + StepFz};
    float R[FOOTPRINT_CORNERS] = {R0, R0 + StepRx, R0 + StepRz, R0 + StepRx + StepRz};
    Out->FzMin = HorizonMin(HorizonMin(Fz[0], Fz[1]), HorizonMin(Fz[2], Fz[3]));
    if (Out->FzMin <= HORIZON_MIN_FZ) { return false; }
    Out->FzMax = HorizonMax(HorizonMax(Fz[0], Fz[1]), HorizonMax(Fz[2], Fz[3]));
    Out->InvFzMin = 1.0F / Out->FzMin;
    Out->InvFzMax = 1.0F / Out->FzMax;
    float U[FOOTPRINT_CORNERS];
    for (int Corner = 0; Corner < FOOTPRINT_CORNERS; Corner++) {
        U[Corner] = R[Corner] / Fz[Corner];
    }
    // Sorting network for four values.
    static const int PAIRS[FOOTPRINT_SORT_STEPS][2] = {
        {0, 1}, {2, 3}, {0, 2}, {1, 3}, {1, 2}};
    for (int Pair = 0; Pair < FOOTPRINT_SORT_STEPS; Pair++) {
        int A = PAIRS[Pair][0];
        int B = PAIRS[Pair][1];
        if (U[A] > U[B]) {
            float Swap = U[A];
            U[A] = U[B];
            U[B] = Swap;
        }
    }
    Out->OuterMin = U[0];
    Out->InnerMin = U[1];
    Out->InnerMax = U[2];
    Out->OuterMax = U[3];
    return true;
}

// Smallest and largest v of a point at height offset Dy over the footprint.
static float FootprintSlopeLow(const HorizonFootprint *F, float Dy) {
    return Dy * ((Dy >= 0.0F) ? F->InvFzMax : F->InvFzMin);
}

static float FootprintSlopeHigh(const HorizonFootprint *F, float Dy) {
    return Dy * ((Dy >= 0.0F) ? F->InvFzMin : F->InvFzMax);
}

// The bins the footprint touches; false when it reaches past the frustum's u
// range, where nothing is recorded.
static bool FootprintBins(const HorizonFrame *H, const HorizonFootprint *F,
                          int *OutFirst, int *OutLast) {
    if (F->OuterMin < H->Umin || F->OuterMax > H->Umax) { return false; }
    int First = (int)((F->OuterMin - H->Umin) * H->InvBinWidth);
    int Last = (int)((F->OuterMax - H->Umin) * H->InvBinWidth);
    *OutFirst = (First < 0) ? 0 : First;
    *OutLast = (Last >= HORIZON_BINS) ? HORIZON_BINS - 1 : Last;
    return true;
}

// What is blocked in every one of a footprint's bins at once: v below
// PrefixMin, and v inside [FloatLoMax, FloatHiMin]. Gathered once per column so
// each of its chunks is tested with two comparisons; a v range blocked in each
// bin but only by different means there is not recognised, which can only keep
// a chunk, never drop one.
typedef struct {
    float PrefixMin;
    float FloatLoMax;
    float FloatHiMin;
} HorizonSpan;

static HorizonSpan GatherHorizonSpan(const HorizonFrame *H, int First, int Last) {
    HorizonSpan Span = {HUGE_VALF, -HUGE_VALF, HUGE_VALF};
    for (int Bin = First; Bin <= Last; Bin++) {
        const HorizonBin *Entry = &H->Bins[Bin];
        Span.PrefixMin = HorizonMin(Span.PrefixMin, Entry->Prefix);
        Span.FloatLoMax = HorizonMax(Span.FloatLoMax, Entry->FloatLo);
        Span.FloatHiMin = HorizonMin(Span.FloatHiMin, Entry->FloatHi);
    }
    return Span;
}

// False when no bin can block every line with v in [VMin, VMax], judged from
// the frame-wide bounds alone.
static bool HorizonMightHide(const HorizonFrame *H, float VMin, float VMax) {
    if (VMax <= H->PrefixMax - HORIZON_SLOPE_EPS) { return true; }
    return VMin >= H->FloatLoMin + HORIZON_SLOPE_EPS &&
           VMax <= H->FloatHiMax - HORIZON_SLOPE_EPS;
}

// Whether every line of the span with v in [VMin, VMax] is blocked.
static bool HorizonSpanHides(const HorizonSpan *Span, float VMin, float VMax) {
    if (VMax <= Span->PrefixMin - HORIZON_SLOPE_EPS) { return true; }
    return VMin >= Span->FloatLoMax + HORIZON_SLOPE_EPS &&
           VMax <= Span->FloatHiMin - HORIZON_SLOPE_EPS;
}

static void HorizonInsertBin(HorizonFrame *H, int Bin, float VBot, float VTop) {
    HorizonBin *Entry = &H->Bins[Bin];
    float Prefix = Entry->Prefix;
    float Lo = Entry->FloatLo;
    float Hi = Entry->FloatHi;
    bool HasFloat = Lo <= Hi;
    if (VBot <= Prefix) {
        Prefix = HorizonMax(Prefix, VTop);
    } else if (HasFloat && VBot <= Hi && VTop >= Lo) {
        Lo = HorizonMin(Lo, VBot);
        Hi = HorizonMax(Hi, VTop);
    } else if (!HasFloat || VTop > Hi) {
        // Disjoint from both: keep the band nearer the horizon line, where the
        // far columns that remain to be tested sit.
        Lo = VBot;
        Hi = VTop;
    }
    if (Lo <= Hi && Lo <= Prefix) {
        Prefix = HorizonMax(Prefix, Hi);
        Lo = HUGE_VALF;
        Hi = -HUGE_VALF;
    }
    Entry->Prefix = Prefix;
    Entry->FloatLo = Lo;
    Entry->FloatHi = Hi;
    H->PrefixMax = HorizonMax(H->PrefixMax, Prefix);
    if (Lo <= Hi) {
        H->FloatLoMin = HorizonMin(H->FloatLoMin, Lo);
        H->FloatHiMax = HorizonMax(H->FloatHiMax, Hi);
    }
}

// Records that every line through the footprint's inner u range with v in
// [VBot, VTop] is blocked.
static void HorizonInsert(HorizonFrame *H, const HorizonFootprint *F, float VBot,
                          float VTop) {
    VBot += HORIZON_SLOPE_EPS;
    VTop -= HORIZON_SLOPE_EPS;
    if (VTop <= VBot) { return; }
    // In bin units, clamped first so the conversions below stay in range.
    float Lo = HorizonMax((F->InnerMin - H->Umin) * H->InvBinWidth, -1.0F);
    float Hi = HorizonMin((F->InnerMax - H->Umin) * H->InvBinWidth,
                          (float)HORIZON_BINS + 1.0F);
    // Only bins wholly inside [Lo, Hi].
    int First = HorizonCeil(Lo);
    int Last = HorizonFloor(Hi) - 1;
    if (First < 0) { First = 0; }
    if (Last >= HORIZON_BINS) { Last = HORIZON_BINS - 1; }
    for (int Bin = First; Bin <= Last; Bin++) { HorizonInsertBin(H, Bin, VBot, VTop); }
}

// A column that survived the frustum, with everything the sweep reads about
// it. Filled during the frustum pass, which walks the columns in memory order;
// the near-to-far pass then reads this compact copy instead of reaching back
// into the column pool in an order that misses the cache on every column.
typedef struct {
    HorizonFootprint Footprint;
    int ChunkX;
    int ChunkZ;
    int MinChunkY;
    int MaxChunkY;
    int Head;
    int Key;
    int16_t RunTop[COLUMN_OCCLUDER_RUNS];
    int16_t RunBottom[COLUMN_OCCLUDER_RUNS];
    uint8_t RunCount;
    bool TestEach;
    // Footprint holds the column's projection: set when the column was tested,
    // which every column is before it can be committed as an occluder.
    bool Projected;
} HorizonCandidate;

// Columns can outnumber the loaded disc while the load centre moves and old
// chunks wait for eviction; a frame with more than this goes without the cull.
enum { HORIZON_MAX_CANDIDATES = 2 * MAX_LOADED_COLUMNS };

// Adds a column's opaque layer runs to the horizon, lowest first so the upper
// run can chain onto the band the lower one extends.
static void HorizonAddOccluder(HorizonFrame *H, const HorizonCandidate *C) {
    if (C->RunCount == 0 || !C->Projected || C->Footprint.FzMin < (float)CHUNK_SIZE) {
        return;
    }
    for (int Run = C->RunCount - 1; Run >= 0; Run--) {
        float Bottom = ((float)C->RunBottom[Run] - BLOCK_HALF_SIZE) - H->CamY;
        float Top = ((float)C->RunTop[Run] + BLOCK_HALF_SIZE) - H->CamY;
        HorizonInsert(H, &C->Footprint, FootprintSlopeHigh(&C->Footprint, Bottom),
                      FootprintSlopeLow(&C->Footprint, Top));
    }
}

// Per key, the smallest key at which a column of that key may hide anything:
// (sqrt(Key) + HORIZON_COMMIT_LAG)^2, with the root rounded up.
static const int *GetHorizonCommitKeys(void) {
    static int Keys[HORIZON_KEY_BUCKETS];
    static bool Built = false;
    if (!Built) {
        int Root = 0;
        for (int Key = 0; Key < HORIZON_KEY_BUCKETS; Key++) {
            while (Root * Root < Key) { Root++; }
            int Reach = Root + HORIZON_COMMIT_LAG;
            Keys[Key] = Reach * Reach;
        }
        Built = true;
    }
    return Keys;
}

static int HorizonColumnKey(const RenderColumn *Col, int CameraChunkX, int CameraChunkZ) {
    int DeltaX = Col->ChunkX - CameraChunkX;
    int DeltaZ = Col->ChunkZ - CameraChunkZ;
    // Compared before squaring so a camera far from the loaded set cannot
    // overflow the product.
    if (abs(DeltaX) >= MAX_RENDER_DISTANCE || abs(DeltaZ) >= MAX_RENDER_DISTANCE) {
        return HORIZON_KEY_BUCKETS - 1;
    }
    int Key = (DeltaX * DeltaX) + (DeltaZ * DeltaZ);
    return (Key >= HORIZON_KEY_BUCKETS) ? HORIZON_KEY_BUCKETS - 1 : Key;
}

// Counting sort of the candidates by key: Out receives their indices, near to
// far, ties in candidate order.
static void SortHorizonCandidates(const HorizonCandidate *Candidates, int Count,
                                  int *Out) {
    static int KeyOffsets[HORIZON_KEY_BUCKETS];
    memset(KeyOffsets, 0, sizeof(KeyOffsets));
    for (int Idx = 0; Idx < Count; Idx++) { KeyOffsets[Candidates[Idx].Key]++; }
    int Running = 0;
    for (int Key = 0; Key < HORIZON_KEY_BUCKETS; Key++) {
        int Here = KeyOffsets[Key];
        KeyOffsets[Key] = Running;
        Running += Here;
    }
    for (int Idx = 0; Idx < Count; Idx++) {
        Out[KeyOffsets[Candidates[Idx].Key]++] = Idx;
    }
}

// What the column sweep reads and fills: the frame's frustum and stats, and
// the visible list with the distance bucket counts every pass sorts on.
typedef struct {
    World *WorldVal;
    RenderableSet *Renderable;
    const RenderFrustum *Frustum;
    RenderCullStats *Stats;
    ChunkOrderEntry *Visible;
    int *SetOrder;
    // Filled only when ties are ordered (scripted runs), else NULL.
    uint32_t *TieKeys;
    int *BucketCounts;
    int VisibleCount;
    int CameraChunkX;
    int CameraChunkY;
    int CameraChunkZ;
} CullSweep;

static void AppendVisibleChunk(CullSweep *S, int Record, int ChunkX, int ChunkY,
                               int ChunkZ) {
    ChunkOrderEntry *Entry = &S->Visible[S->VisibleCount];
    Entry->Record = Record;
    Entry->Bucket = ChunkDistanceBucket(ChunkX - S->CameraChunkX, ChunkY - S->CameraChunkY,
                                        ChunkZ - S->CameraChunkZ);
    S->BucketCounts[Entry->Bucket]++;
    S->SetOrder[S->VisibleCount] = Record;
    if (S->TieKeys != NULL) {
        S->TieKeys[S->VisibleCount] = ChunkTieKey(ChunkX - S->CameraChunkX,
                                                  ChunkY - S->CameraChunkY,
                                                  ChunkZ - S->CameraChunkZ);
    }
    S->VisibleCount++;
}

// A column the frustum does not rule out, and whether its chunks still need
// testing one by one.
typedef struct {
    int Column;
    bool TestEach;
} SweepColumn;

// A column wholly outside or wholly inside the frustum settles all its chunks
// with one box test; only straddling columns test their chunks one by one.
// Within a tile wholly inside, the column is inside without a test of its own.
static BoxClass ClassifySweepColumn(CullSweep *S, int Column, BoxClass TileClass) {
    RenderColumn *Col = &S->Renderable->Columns[Column];
    if (Col->ExtentStale) { RefreshColumnExtent(S->Renderable, Col); }
    BoxClass Class = TileClass;
    if (Class != BOX_INSIDE) {
        Vec3 BoxMin;
        Vec3 BoxMax;
        GetColumnBox(Col, &BoxMin, &BoxMax);
        Class = ClassifyColumnBox(S->Frustum, BoxMin, BoxMax);
    }
    if (Class == BOX_OUTSIDE) {
        S->Stats->ColumnsOutside++;
    } else if (Class == BOX_INTERSECTING) {
        S->Stats->ColumnsIntersecting++;
    } else {
        S->Stats->ColumnsInside++;
    }
    return Class;
}

static void GatherSweepColumn(CullSweep *S, int Column, BoxClass TileClass,
                              SweepColumn *Out, int *Count) {
    BoxClass Class = ClassifySweepColumn(S, Column, TileClass);
    if (Class == BOX_OUTSIDE) { return; }
    Out[*Count] = (SweepColumn){Column, Class == BOX_INTERSECTING};
    (*Count)++;
}

// The columns the frustum leaves, tile by tile: a tile wholly outside drops
// all its columns untouched, one wholly inside keeps them all untested.
// While any column is untiled, every column is tested in set order instead.
static int GatherSweepColumns(CullSweep *S, SweepColumn *Out) {
    RenderableSet *Set = S->Renderable;
    int Count = 0;
    if (Set->UntiledColumns > 0) {
        for (int Dense = 0; Dense < Set->ActiveColumnCount; Dense++) {
            GatherSweepColumn(S, Set->ActiveColumns[Dense], BOX_INTERSECTING, Out, &Count);
        }
        return Count;
    }
    for (int Dense = 0; Dense < Set->ActiveTileCount; Dense++) {
        RenderTile *Entry = &Set->Tiles[Set->ActiveTiles[Dense]];
        if (Entry->ExtentStale) { RefreshTileExtent(Set, Entry); }
        float TileSpan = (float)(RENDER_TILE_SIDE * CHUNK_SIZE);
        Vec3 BoxMin = {
            ((float)(Entry->TileX * RENDER_TILE_SIDE * CHUNK_SIZE)) - BLOCK_HALF_SIZE,
            ((float)(Entry->MinChunkY * CHUNK_SIZE)) - BLOCK_HALF_SIZE,
            ((float)(Entry->TileZ * RENDER_TILE_SIDE * CHUNK_SIZE)) - BLOCK_HALF_SIZE,
        };
        Vec3 BoxMax = {
            BoxMin.x + TileSpan,
            ((float)((Entry->MaxChunkY + 1) * CHUNK_SIZE)) - BLOCK_HALF_SIZE,
            BoxMin.z + TileSpan,
        };
        BoxClass TileClass = ClassifyColumnBox(S->Frustum, BoxMin, BoxMax);
        if (TileClass == BOX_OUTSIDE) {
            S->Stats->ColumnsOutside += Entry->Count;
            continue;
        }
        for (int Column = Entry->Head; Column != 0; Column = Set->Columns[Column].TileNext) {
            GatherSweepColumn(S, Column, TileClass, Out, &Count);
        }
    }
    return Count;
}

// Sized like the horizon's candidates: the loaded disc twice over. Columns can
// outnumber it only while the load centre moves and old chunks await eviction;
// such a frame streams the columns instead of collecting them.
enum { SWEEP_COLUMN_CAPACITY = 2 * MAX_LOADED_COLUMNS };
_Static_assert(SWEEP_COLUMN_CAPACITY == HORIZON_MAX_CANDIDATES,
               "the horizon sweep relies on the column list holding every column");

static SweepColumn *GetSweepColumns(void) {
    static SweepColumn Columns[SWEEP_COLUMN_CAPACITY];
    return Columns;
}

// One column's chunks, with frustum culling when the column straddles it and,
// when the traversal ran, the chunks it reached.
static void SweepColumnChunks(CullSweep *S, const RenderColumn *Col, bool TestEach,
                              bool OcclusionActive) {
    const RenderableSet *Renderable = S->Renderable;
    for (int Rec = Col->Head; Rec != 0; Rec = Renderable->Records[Rec].NextInColumn) {
        const RenderableRecord *Entry = &Renderable->Records[Rec];
        if (TestEach) {
            S->Stats->ChunksTested++;
            if (!IsChunkCellVisible(S->Frustum, Col->ChunkX, Entry->ChunkY, Col->ChunkZ)) {
                continue;
            }
        }
        if (OcclusionActive && !WasSlotReached(Entry->Slot)) {
            S->Stats->Occluded++;
            continue;
        }
        AppendVisibleChunk(S, Rec, Col->ChunkX, Entry->ChunkY, Col->ChunkZ);
    }
}

// The columns in set order, with frustum culling and, when the traversal ran,
// the chunks it reached.
static void SweepFrustumOnly(CullSweep *S, bool OcclusionActive) {
    RenderableSet *Renderable = S->Renderable;
    if (Renderable->ActiveColumnCount > SWEEP_COLUMN_CAPACITY) {
        for (int Dense = 0; Dense < Renderable->ActiveColumnCount; Dense++) {
            int Column = Renderable->ActiveColumns[Dense];
            BoxClass Class = ClassifySweepColumn(S, Column, BOX_INTERSECTING);
            if (Class == BOX_OUTSIDE) { continue; }
            SweepColumnChunks(S, &Renderable->Columns[Column], Class == BOX_INTERSECTING,
                              OcclusionActive);
        }
        return;
    }
    SweepColumn *Columns = GetSweepColumns();
    int ColumnCount = GatherSweepColumns(S, Columns);
    for (int Idx = 0; Idx < ColumnCount; Idx++) {
        SweepColumnChunks(S, &Renderable->Columns[Columns[Idx].Column],
                          Columns[Idx].TestEach, OcclusionActive);
    }
}

// The surviving columns near to far, each tested against the horizon of the
// columns committed before it and then queued to join it.
static void SweepWithHorizon(CullSweep *S, HorizonFrame *H) {
    static HorizonCandidate Candidates[HORIZON_MAX_CANDIDATES];
    static int Order[HORIZON_MAX_CANDIDATES];
    RenderableSet *Renderable = S->Renderable;
    SweepColumn *Columns = GetSweepColumns();
    int ColumnCount = GatherSweepColumns(S, Columns);
    int Count = 0;
    for (int Idx = 0; Idx < ColumnCount; Idx++) {
        RenderColumn *Col = &Renderable->Columns[Columns[Idx].Column];
        if (Col->RunsStale) { RefreshColumnRuns(S->WorldVal, Col); }
        HorizonCandidate *C = &Candidates[Count++];
        C->ChunkX = Col->ChunkX;
        C->ChunkZ = Col->ChunkZ;
        C->MinChunkY = Col->MinChunkY;
        C->MaxChunkY = Col->MaxChunkY;
        C->Head = Col->Head;
        C->Key = HorizonColumnKey(Col, S->CameraChunkX, S->CameraChunkZ);
        C->RunCount = Col->RunCount;
        for (int Run = 0; Run < COLUMN_OCCLUDER_RUNS; Run++) {
            C->RunTop[Run] = Col->RunTop[Run];
            C->RunBottom[Run] = Col->RunBottom[Run];
        }
        C->TestEach = Columns[Idx].TestEach;
        C->Projected = false;
    }
    SortHorizonCandidates(Candidates, Count, Order);

    const int *CommitKeys = GetHorizonCommitKeys();
    int Committed = 0;
    for (int Idx = 0; Idx < Count; Idx++) {
        HorizonCandidate *C = &Candidates[Order[Idx]];
        while (Committed < Idx &&
               CommitKeys[Candidates[Order[Committed]].Key] <= C->Key) {
            HorizonAddOccluder(H, &Candidates[Order[Committed]]);
            Committed++;
        }

        C->Projected = ProjectFootprint(H, C->ChunkX, C->ChunkZ, &C->Footprint);
        int FirstBin = 0;
        int LastBin = -1;
        bool Testable = C->Projected && FootprintBins(H, &C->Footprint, &FirstBin, &LastBin);
        HorizonSpan Span = {0};
        bool SpanReady = false;
        bool ColumnHidden = false;
        if (Testable) {
            float Bottom = ((float)(C->MinChunkY * CHUNK_SIZE) - BLOCK_HALF_SIZE) - H->CamY;
            float Top = ((float)((C->MaxChunkY + 1) * CHUNK_SIZE) - BLOCK_HALF_SIZE) - H->CamY;
            float VMin = FootprintSlopeLow(&C->Footprint, Bottom);
            float VMax = FootprintSlopeHigh(&C->Footprint, Top);
            if (HorizonMightHide(H, VMin, VMax)) {
                Span = GatherHorizonSpan(H, FirstBin, LastBin);
                SpanReady = true;
                ColumnHidden = HorizonSpanHides(&Span, VMin, VMax);
            }
        }

        for (int Rec = C->Head; Rec != 0; Rec = Renderable->Records[Rec].NextInColumn) {
            int ChunkY = Renderable->Records[Rec].ChunkY;
            if (C->TestEach) {
                S->Stats->ChunksTested++;
                if (!IsChunkCellVisible(S->Frustum, C->ChunkX, ChunkY, C->ChunkZ)) { continue; }
            }
            if (Testable) {
                bool Hidden = ColumnHidden;
                if (!Hidden) {
                    float Bottom = ((float)(ChunkY * CHUNK_SIZE) - BLOCK_HALF_SIZE) -
                                   H->CamY;
                    float VMin = FootprintSlopeLow(&C->Footprint, Bottom);
                    float VMax = FootprintSlopeHigh(&C->Footprint, Bottom + (float)CHUNK_SIZE);
                    if (HorizonMightHide(H, VMin, VMax)) {
                        if (!SpanReady) {
                            Span = GatherHorizonSpan(H, FirstBin, LastBin);
                            SpanReady = true;
                        }
                        Hidden = HorizonSpanHides(&Span, VMin, VMax);
                    }
                }
                if (Hidden) {
                    S->Stats->HorizonCulled++;
                    continue;
                }
            }
            AppendVisibleChunk(S, Rec, C->ChunkX, ChunkY, C->ChunkZ);
        }
    }
}

void DrawWorld(World *WorldVal, RenderCamera CameraVal) {
    PROFILE_BEGIN(DrawStart);
    if (GetDebugState()->Wireframe) { RenderSetWireframe(true); }

    // Once per frame rather than once per chunk. The previous test normalised
    // the camera forward vector inside itself, so it was recomputed for every
    // chunk on every pass.
    RenderFrustum Frustum;
    BuildRenderFrustum(&Frustum);

    static ChunkOrderEntry VisibleChunks[MAX_ACTIVE_CHUNKS];
    static int NearToFar[MAX_ACTIVE_CHUNKS];
    static int SetOrder[MAX_ACTIVE_CHUNKS];
    static uint32_t VisibleTieKeys[MAX_ACTIVE_CHUNKS];
    static uint32_t NearToFarTieKeys[MAX_ACTIVE_CHUNKS];
    bool StableOrder = GetDebugState()->StableDrawOrder;

    // Static: two arrays of CHUNK_DISTANCE_BUCKETS ints are too large for the
    // stack, and only the render thread draws.
    static int BucketCounts[CHUNK_DISTANCE_BUCKETS];
    static int Offsets[CHUNK_DISTANCE_BUCKETS];
    memset(BucketCounts, 0, sizeof(BucketCounts));
    int VisibleCount = 0;
    RenderableSet *Renderable = GetRenderableSet();
    RenderCullStats Stats = {
        .Renderable = Renderable->RecordCount,
        .Columns = Renderable->ActiveColumnCount,
        .OcclusionEnabled = GetDebugState()->Occlusion,
    };

    // Stamps every chunk a sight line from the camera can reach. On fallback
    // (camera outside the loaded set) the frame keeps frustum culling alone.
    bool OcclusionActive = false;
    if (Stats.OcclusionEnabled) {
        PROFILE_BEGIN(OcclusionStart);
        OcclusionActive = RunOcclusionTraversal(WorldVal, &Frustum, CameraVal.Position);
        PROFILE_END(OcclusionStart, PROFILE_OCCLUSION);
        const OcclusionState *Occlusion = GetOcclusionState();
        Stats.OcclusionFallback = !OcclusionActive;
        Stats.OcclusionNodes = OcclusionActive ? Occlusion->Nodes : 0;
        Stats.OcclusionReachable = OcclusionActive ? Occlusion->Reachable : 0;
    }

    // The camera's own chunk, so the per-chunk work below is integer subtraction
    // rather than anything in world space.
    int CameraChunkX = (int)__builtin_floorf(CameraVal.Position.x / CHUNK_SIZE);
    int CameraChunkY = (int)__builtin_floorf(CameraVal.Position.y / CHUNK_SIZE);
    int CameraChunkZ = (int)__builtin_floorf(CameraVal.Position.z / CHUNK_SIZE);

    // Visibility and distance in one sweep over the columns holding a mesh;
    // mesh-less slots are never visited. A column wholly outside or wholly
    // inside the frustum settles all its chunks with one box test; only
    // straddling columns test their chunks one by one. The bucket is what
    // every pass sorts on, so deriving it here costs three integer multiplies
    // per visible chunk per frame rather than one per chunk per pass.
    //
    // With the horizon cull active the surviving columns are walked near to
    // far instead of in set order; otherwise the order is left as it was.
    PROFILE_BEGIN(SweepStart);
    HorizonFrame *Horizon = GetHorizonFrame();
    Stats.HorizonEnabled = Stats.OcclusionEnabled && GetDebugState()->HorizonCulling;
    bool HorizonActive = Stats.HorizonEnabled && !OcclusionActive &&
                         !GetOcclusionState()->CameraEnclosed &&
                         Renderable->ActiveColumnCount <= HORIZON_MAX_CANDIDATES &&
                         BeginHorizonFrame(Horizon, &Frustum, CameraVal);
    Stats.HorizonActive = HorizonActive;

    CullSweep Sweep = {
        .WorldVal = WorldVal,
        .Renderable = Renderable,
        .Frustum = &Frustum,
        .Stats = &Stats,
        .Visible = VisibleChunks,
        .SetOrder = SetOrder,
        .BucketCounts = BucketCounts,
        .VisibleCount = 0,
        .CameraChunkX = CameraChunkX,
        .CameraChunkY = CameraChunkY,
        .CameraChunkZ = CameraChunkZ,
        .TieKeys = StableOrder ? VisibleTieKeys : NULL,
    };
    if (HorizonActive) {
        SweepWithHorizon(&Sweep, Horizon);
    } else {
        SweepFrustumOnly(&Sweep, OcclusionActive);
    }
    VisibleCount = Sweep.VisibleCount;
    // Frustum candidates, whether or not occlusion or the horizon then removed
    // them.
    Stats.Visible = VisibleCount + Stats.Occluded + Stats.HorizonCulled;
    PROFILE_END(SweepStart, PROFILE_CULL_SWEEP);

    // Counting sort walked from the near bucket up, so the output runs nearest
    // to farthest. The translucent pass reads the same array backwards.
    int Running = 0;
    for (int B = 0; B < CHUNK_DISTANCE_BUCKETS; B++) {
        Offsets[B] = Running;
        Running += BucketCounts[B];
    }
    for (int IdxI = 0; IdxI < VisibleCount; IdxI++) {
        int Pos = Offsets[VisibleChunks[IdxI].Bucket]++;
        NearToFar[Pos] = VisibleChunks[IdxI].Record;
        if (StableOrder) { NearToFarTieKeys[Pos] = VisibleTieKeys[IdxI]; }
    }
    if (StableOrder) { SortDistanceTies(NearToFar, NearToFarTieKeys, Offsets, BucketCounts); }
    RenderSetExactChunkOrder(StableOrder);

    // The solid and cutout passes are the ones the toggle governs; SetOrder is
    // the visible chunks in whatever order the renderable set holds them, the
    // unsorted traversal the toggle compares against.
    const int *SolidOrder = GetDebugState()->SortChunks ? NearToFar : SetOrder;

    int Rendered = 0;

    // Solid faces, nearest first, with the shader that keeps early depth
    // rejection. Order is what gives that rejection something to reject: an
    // occluded chunk drawn before its occluder is shaded and then overwritten.
    //
    // The ForceCutoutShader toggle routes them back through the cutout shader,
    // restoring the pre-split behaviour so the two can be measured against each
    // other.
    //
    // Each pass is timed into "Chunk submit", nested inside "World draw", so the
    // submission cost reads apart from the culling and ordering above.
    PROFILE_BEGIN(SolidSubmitStart);
    RenderBeginChunkPass(GetDebugState()->ForceCutoutShader ? CHUNK_PASS_CUTOUT
                                                            : CHUNK_PASS_OPAQUE);
    for (int IdxI = 0; IdxI < VisibleCount; IdxI++) {
        const RenderableRecord *Entry = &Renderable->Records[SolidOrder[IdxI]];
        MeshHandle Mesh = Entry->Meshes[MESH_TARGET_OPAQUE];
        if (Mesh == MESH_HANDLE_INVALID) { continue; }
        RenderDrawMesh(Mesh);
        Stats.SubmittedVertices += Entry->VertexCounts[MESH_TARGET_OPAQUE];
        Rendered++;
    }
    RenderEndChunkPass();
    PROFILE_END(SolidSubmitStart, PROFILE_CHUNK_SUBMIT);

    // Alpha-cutout flora, which needs the test the opaque shader drops. Ordered
    // with the solid pass; it runs after it and tests against the depth it wrote.
    PROFILE_BEGIN(CutoutSubmitStart);
    RenderBeginChunkPass(CHUNK_PASS_CUTOUT);
    for (int IdxI = 0; IdxI < VisibleCount; IdxI++) {
        const RenderableRecord *Entry = &Renderable->Records[SolidOrder[IdxI]];
        MeshHandle Mesh = Entry->Meshes[MESH_TARGET_CUTOUT];
        if (Mesh == MESH_HANDLE_INVALID) { continue; }
        RenderDrawMesh(Mesh);
        Stats.SubmittedVertices += Entry->VertexCounts[MESH_TARGET_CUTOUT];
    }
    RenderEndChunkPass();
    PROFILE_END(CutoutSubmitStart, PROFILE_CHUNK_SUBMIT);

    // Immediate-mode, and it binds its own shader and texture, so it stays
    // outside the chunk passes rather than being interleaved with them. It
    // outlines mesh-less chunks too, so it keeps its own sweep over every slot,
    // paid only while the overlay is on.
    if (GetDebugState()->ChunkBorders) {
        for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
            const Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
            if (!WorldVal->SlotInUse[IdxI] || !IsChunkVisible(&Frustum, ChunkVal)) {
                continue;
            }
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
    PROFILE_BEGIN(TranslucentSubmitStart);
    RenderBeginTranslucentPass();

    RenderBeginChunkPass(CHUNK_PASS_CUTOUT);
    for (int IdxI = VisibleCount - 1; IdxI >= 0; IdxI--) {
        const RenderableRecord *Entry = &Renderable->Records[NearToFar[IdxI]];
        MeshHandle Mesh = Entry->Meshes[MESH_TARGET_TRANSLUCENT];
        if (Mesh == MESH_HANDLE_INVALID) { continue; }
        RenderDrawMesh(Mesh);
        Stats.SubmittedVertices += Entry->VertexCounts[MESH_TARGET_TRANSLUCENT];
    }
    RenderEndChunkPass();

    RenderEndTranslucentPass();
    PROFILE_END(TranslucentSubmitStart, PROFILE_CHUNK_SUBMIT);

    Stats.Rendered = Rendered;
    *GetCullStatsState() = Stats;

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
