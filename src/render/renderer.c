#include "render/renderer.h"
#include "render/backend.h"
#include "world/block_system.h"
#include "world/chunk.h"
#include "ui/debug.h"
#include "core/vecmath.h"
#include <stddef.h>
#include <stdbool.h>

#define VERTICES_PER_FACE 4
#define FLOATS_PER_VERTEX 3
#define COLOR_CHANNELS 4
#define OPAQUE_ALPHA_VALUE 255
#define CHUNK_CLOSE_DISTANCE_FACTOR 2.0F
#define INDICES_PER_FACES 6
#define MAX_FACES (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * INDICES_PER_FACES)

#define CHUNK_SPHERE_RADIUS 14.0F
#define FRUSTUM_DOT_THRESHOLD 0.4F

// Backend-agnostic mesh-builder scratch. Filled per chunk, then handed to the
// render backend as MeshData. Holds no renderer (Raylib) types.
typedef struct {
  float TempTexCoords[MAX_FACES * VERTICES_PER_FACE * 2];
  float TempVertices[MAX_FACES * VERTICES_PER_FACE * FLOATS_PER_VERTEX];
  unsigned short TempIndices[MAX_FACES * INDICES_PER_FACES];
  unsigned char TempColors[MAX_FACES * VERTICES_PER_FACE * COLOR_CHANNELS];
  float TempTexCoords2[MAX_FACES * VERTICES_PER_FACE * 2];

  int VCount;
  int ICount;

  float TransTexCoords[MAX_FACES * VERTICES_PER_FACE * 2];
  float TransVertices[MAX_FACES * VERTICES_PER_FACE * FLOATS_PER_VERTEX];
  unsigned short TransIndices[MAX_FACES * INDICES_PER_FACES];
  unsigned char TransColors[MAX_FACES * VERTICES_PER_FACE * COLOR_CHANNELS];
  float TransTexCoords2[MAX_FACES * VERTICES_PER_FACE * 2];

  int TransVCount;
  int TransICount;
} MesherState;

static MesherState *GetMesherState(void) {
  static MesherState State;
  return &State;
}

void InitRenderer(void) { RenderBackendInit(); }

void CloseRenderer(void) { RenderBackendShutdown(); }

// ---------------------------------------------------------------------------
// AO helpers
// ---------------------------------------------------------------------------

static unsigned char GetBlockIDAtLocal(World *WorldVal, Chunk *ChunkVal, int LocalX, int LocalY, int LocalZ) {
    if (LocalX >= 0 && LocalX < CHUNK_SIZE &&
        LocalY >= 0 && LocalY < CHUNK_SIZE &&
        LocalZ >= 0 && LocalZ < CHUNK_SIZE) {
        return ChunkVal->Data[LocalX][LocalY][LocalZ];
    }
    int Gx = (ChunkVal->ChunkX * CHUNK_SIZE) + LocalX;
    int Gy = (ChunkVal->ChunkY * CHUNK_SIZE) + LocalY;
    int Gz = (ChunkVal->ChunkZ * CHUNK_SIZE) + LocalZ;
    return (unsigned char)GetBlockIDFromWorld(WorldVal, (Vec3){(float)Gx, (float)Gy, (float)Gz});
}

static bool IsNeighbourTransparent(World *WorldVal, Chunk *ChunkVal,
                                   int LocalX, int LocalY, int LocalZ,
                                   unsigned char SelfId) {
    unsigned char Id = GetBlockIDAtLocal(WorldVal, ChunkVal, LocalX, LocalY, LocalZ);
    if (Id == 0) { return true; }
    if (Id == SelfId && GetBlockDef(SelfId)->IsTransparent) { return false; }
    return GetBlockDef(Id)->IsTransparent;
}

static bool IsSolidForAO(World *WorldVal, Chunk *ChunkVal, int LocalX, int LocalY, int LocalZ) {
    unsigned char Id = GetBlockIDAtLocal(WorldVal, ChunkVal, LocalX, LocalY, LocalZ);
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

static bool ComputeFaceAO(World *WorldVal, Chunk *ChunkVal,
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

        bool S1 = IsSolidForAO(WorldVal, ChunkVal, S1x, S1y, S1z);
        bool S2 = IsSolidForAO(WorldVal, ChunkVal, S2x, S2y, S2z);
        bool Corner = IsSolidForAO(WorldVal, ChunkVal, Cx, Cy, Cz);

        if (S1 && S2) { Ao[V] = 3; }
        else { Ao[V] = ((int)S1 + (int)S2 + (int)Corner); }
    }
    return (Ao[0] + Ao[2] > Ao[1] + Ao[3]);
}

// ---------------------------------------------------------------------------
// Mesh builder primitives
// ---------------------------------------------------------------------------

static void AddFaceIndices(bool IsTrans, bool FlipQuad) {
    MesherState *State = GetMesherState();
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

static void AddFaceColors(const int Ao[4], bool IsTrans) {
    MesherState *State = GetMesherState();
    int ColIdx = (IsTrans ? State->TransVCount : State->VCount) * COLOR_CHANNELS;
    unsigned char *ColorsArray = IsTrans ? State->TransColors : State->TempColors;
    #pragma unroll 4
    for (int IdxI = 0; IdxI < 4; IdxI++) {
        unsigned char B = AO_BRIGHTNESS[Ao[IdxI]];
        ColorsArray[ColIdx++] = B;
        ColorsArray[ColIdx++] = B;
        ColorsArray[ColIdx++] = B;
        ColorsArray[ColIdx++] = OPAQUE_ALPHA_VALUE;
    }
}

// UV: raw tile-count coords [0..W, 0..H] — shader + array texture handles tiling
static void AddGreedyFaceTexCoords(float UMax, float VMax, bool IsTrans) {
    MesherState *State = GetMesherState();
    int UvIdx = (IsTrans ? State->TransVCount : State->VCount) * 2;
    float *UvArray = IsTrans ? State->TransTexCoords : State->TempTexCoords;
    UvArray[UvIdx++] = 0.0F; UvArray[UvIdx++] = VMax;
    UvArray[UvIdx++] = UMax; UvArray[UvIdx++] = VMax;
    UvArray[UvIdx++] = UMax; UvArray[UvIdx++] = 0.0F;
    UvArray[UvIdx++] = 0.0F; UvArray[UvIdx++] = 0.0F;
}

static void AddFaceTexLayer(float Layer, bool IsTrans) {
    MesherState *State = GetMesherState();
    int Tc2Idx = (IsTrans ? State->TransVCount : State->VCount) * 2;
    float *Tc2Array = IsTrans ? State->TransTexCoords2 : State->TempTexCoords2;
    #pragma unroll 4
    for (int IdxI = 0; IdxI < 4; IdxI++) {
        Tc2Array[Tc2Idx + (IdxI * 2) + 0] = Layer;
        Tc2Array[Tc2Idx + (IdxI * 2) + 1] = 0.0F;
    }
}

// Greedy quad vertices for a W×H merged face.
static void AddGreedyFaceVertices(BlockFace Face,
                                   int Wx0, int Wy0, int Wz0,
                                   int W, int H, bool IsTrans) {
    MesherState *State = GetMesherState();
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

static void AddGreedyFaceToMeshBuilder(BlockFace Face,
                                        int Wx0, int Wy0, int Wz0,
                                        int W, int H,
                                        int TexIndex, const int Ao[4],
                                        bool FlipQuad, bool IsTrans) {
    MesherState *State = GetMesherState();
    AddFaceIndices(IsTrans, FlipQuad);
    AddGreedyFaceTexCoords((float)W, (float)H, IsTrans);
    AddFaceColors(Ao, IsTrans);
    AddFaceTexLayer((float)TexIndex, IsTrans);
    AddGreedyFaceVertices(Face, Wx0, Wy0, Wz0, W, H, IsTrans);

    if (IsTrans) { State->TransVCount += VERTICES_PER_FACE; }
    else { State->VCount += VERTICES_PER_FACE; }
}

// ---------------------------------------------------------------------------
// FaceMask and greedy scan
// ---------------------------------------------------------------------------

typedef struct {
    unsigned char BlockId;
    unsigned char TexIndex;
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

static bool MasksCompatible(const FaceMaskData *A, const FaceMaskData *B) {
    if (A->BlockId == 0 || B->BlockId == 0) { return false; }
    if (A->Used || B->Used) { return false; }
    if (A->BlockId != B->BlockId) { return false; }
    if (A->TexIndex != B->TexIndex) { return false; }
    if (A->FlipQuad != B->FlipQuad) { return false; }
    #pragma unroll 4
    for (int IdxI = 0; IdxI < VERTICES_PER_FACE; IdxI++) { if (A->Ao[IdxI] != B->Ao[IdxI]) { return false; } }
    return true;
}

static void BuildGreedyFacePass(World *WorldVal, Chunk *ChunkVal, const FaceDir *Fd) {
    for (int Slice = 0; Slice < CHUNK_SIZE; Slice++) {
        FaceMaskData Mask[CHUNK_SIZE][CHUNK_SIZE];

        // Build mask for this slice
        #pragma unroll 4
        for (int U = 0; U < CHUNK_SIZE; U++) {
            #pragma unroll 4
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

                unsigned char Id = ChunkVal->Data[Lx][Ly][Lz];
                if (Id == 0) { continue; }

                BlockType *Def = GetBlockDef(Id);
                if (Def->IsTransparent) { continue; }

                int Nlpos[3] = { Lpos[0], Lpos[1], Lpos[2] };
                Nlpos[Fd->NormalAxis] += Fd->NormalDir;
                if (!IsNeighbourTransparent(WorldVal, ChunkVal,
                                            Nlpos[0], Nlpos[1], Nlpos[2], Id)) { continue; }

                Mask[U][V].BlockId   = Id;
                Mask[U][V].TexIndex  = (unsigned char)GetFaceTex(Def, Fd->Face);
                Mask[U][V].FlipQuad  = ComputeFaceAO(WorldVal, ChunkVal, Lx, Ly, Lz,
                                                     Fd->Face, Mask[U][V].Ao);
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
                    #pragma unroll 4
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
                int Wx0 = (ChunkVal->ChunkX * CHUNK_SIZE) + FirstLpos[0];
                int Wy0 = (ChunkVal->ChunkY * CHUNK_SIZE) + FirstLpos[1];
                int Wz0 = (ChunkVal->ChunkZ * CHUNK_SIZE) + FirstLpos[2];

                AddGreedyFaceToMeshBuilder(Fd->Face, Wx0, Wy0, Wz0, W, H,
                                           Origin->TexIndex, Origin->Ao,
                                           Origin->FlipQuad, false);

                // Mark used
                #pragma unroll 4
                for (int Ku = 0; Ku < W; Ku++) {
                    #pragma unroll 4
                    for (int Kv = 0; Kv < H; Kv++) {
                        Mask[U0 + Ku][V0 + Kv].Used = true;
                    }
                }
            }
        }
    }
}

// Per-face pass for transparent blocks (no greedy merging)
static void BuildTransparentFacePass(World *WorldVal, Chunk *ChunkVal) {
    #pragma unroll 4
    for (int X = 0; X < CHUNK_SIZE; X++) {
        #pragma unroll 4
        for (int Y = 0; Y < CHUNK_SIZE; Y++) {
            #pragma unroll 4
            for (int Z = 0; Z < CHUNK_SIZE; Z++) {
                unsigned char Id = ChunkVal->Data[X][Y][Z];
                if (Id == 0) { continue; }

                BlockType *Def = GetBlockDef(Id);
                if (!Def->IsTransparent) { continue; }

                int Ao[4];
                bool Flip;

                int Wx = (ChunkVal->ChunkX * CHUNK_SIZE) + X;
                int Wy = (ChunkVal->ChunkY * CHUNK_SIZE) + Y;
                int Wz = (ChunkVal->ChunkZ * CHUNK_SIZE) + Z;

                if (IsNeighbourTransparent(WorldVal, ChunkVal, X, Y+1, Z, Id)) {
                    Flip = ComputeFaceAO(WorldVal, ChunkVal, X, Y, Z, FACE_TOP, Ao);
                    AddGreedyFaceToMeshBuilder(FACE_TOP, Wx, Wy, Wz, 1, 1,
                                               Def->TexTop, Ao, Flip, true);
                }
                if (IsNeighbourTransparent(WorldVal, ChunkVal, X, Y-1, Z, Id)) {
                    Flip = ComputeFaceAO(WorldVal, ChunkVal, X, Y, Z, FACE_BOTTOM, Ao);
                    AddGreedyFaceToMeshBuilder(FACE_BOTTOM, Wx, Wy, Wz, 1, 1,
                                               Def->TexBottom, Ao, Flip, true);
                }
                if (IsNeighbourTransparent(WorldVal, ChunkVal, X+1, Y, Z, Id)) {
                    Flip = ComputeFaceAO(WorldVal, ChunkVal, X, Y, Z, FACE_RIGHT, Ao);
                    AddGreedyFaceToMeshBuilder(FACE_RIGHT, Wx, Wy, Wz, 1, 1,
                                               Def->TexSide, Ao, Flip, true);
                }
                if (IsNeighbourTransparent(WorldVal, ChunkVal, X-1, Y, Z, Id)) {
                    Flip = ComputeFaceAO(WorldVal, ChunkVal, X, Y, Z, FACE_LEFT, Ao);
                    AddGreedyFaceToMeshBuilder(FACE_LEFT, Wx, Wy, Wz, 1, 1,
                                               Def->TexSide, Ao, Flip, true);
                }
                if (IsNeighbourTransparent(WorldVal, ChunkVal, X, Y, Z+1, Id)) {
                    Flip = ComputeFaceAO(WorldVal, ChunkVal, X, Y, Z, FACE_FRONT, Ao);
                    AddGreedyFaceToMeshBuilder(FACE_FRONT, Wx, Wy, Wz, 1, 1,
                                               Def->TexSide, Ao, Flip, true);
                }
                if (IsNeighbourTransparent(WorldVal, ChunkVal, X, Y, Z-1, Id)) {
                    Flip = ComputeFaceAO(WorldVal, ChunkVal, X, Y, Z, FACE_BACK, Ao);
                    AddGreedyFaceToMeshBuilder(FACE_BACK, Wx, Wy, Wz, 1, 1,
                                               Def->TexSide, Ao, Flip, true);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// BuildChunkMesh
// ---------------------------------------------------------------------------

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

void BuildChunkMesh(World *WorldVal, Chunk *ChunkVal) {
    MesherState *State = GetMesherState();
    if (ChunkVal->SolidBlockCount == 0) {
        UnloadChunkMesh(ChunkVal);
        ChunkVal->IsDirty = false;
        return;
    }

    State->VCount = 0; State->ICount = 0;
    State->TransVCount = 0; State->TransICount = 0;

    // Opaque greedy pass
    static const int FACE_DIRS_COUNT = (int)(sizeof(FACE_DIRS) / sizeof(FACE_DIRS[0]));
    #pragma unroll 4
    for (int Fd = 0; Fd < FACE_DIRS_COUNT; Fd++) { BuildGreedyFacePass(WorldVal, ChunkVal, &FACE_DIRS[Fd]); }

    // Transparent per-face pass
    BuildTransparentFacePass(WorldVal, ChunkVal);

    UnloadChunkMesh(ChunkVal);
    ChunkVal->IsDirty = false;

    // Upload opaque mesh
    if (State->VCount == 0) {
        ChunkVal->HasMesh = false;
    } else {
        MeshData Data = {
            .Vertices = State->TempVertices,
            .Indices = State->TempIndices,
            .TexCoords = State->TempTexCoords,
            .Colors = State->TempColors,
            .TexLayers = State->TempTexCoords2,
            .VertexCount = State->VCount,
            .IndexCount = State->ICount,
        };
        ChunkVal->ChunkMesh = RenderUploadMesh(&Data);
        ChunkVal->HasMesh = (ChunkVal->ChunkMesh != MESH_HANDLE_INVALID);
    }

    // Upload translucent mesh
    if (State->TransVCount == 0) {
        ChunkVal->HasTranslucentMesh = false;
    } else {
        MeshData Data = {
            .Vertices = State->TransVertices,
            .Indices = State->TransIndices,
            .TexCoords = State->TransTexCoords,
            .Colors = State->TransColors,
            .TexLayers = State->TransTexCoords2,
            .VertexCount = State->TransVCount,
            .IndexCount = State->TransICount,
        };
        ChunkVal->TranslucentMesh = RenderUploadMesh(&Data);
        ChunkVal->HasTranslucentMesh = (ChunkVal->TranslucentMesh != MESH_HANDLE_INVALID);
    }
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
    if (GetDebugState()->Wireframe) { RenderSetWireframe(true); }

    // Opaque pass
    #pragma unroll 4
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

    #pragma unroll 4
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
    #pragma unroll 4
    for (int IdxI = 0; IdxI < VisibleCount; IdxI++) {
        RenderDrawMesh(WorldVal->Chunks[VisibleChunks[IdxI].Index].TranslucentMesh);
    }

    RenderEndTranslucentPass();

    if (GetDebugState()->Wireframe) { RenderSetWireframe(false); }
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

    #pragma unroll 4
    for (int IdxI = 0; IdxI < COLLISION_POINTS; IdxI++) {
        RenderDrawDebugCube(BottomPoints[IdxI], Sz, false, IsPointSolid(WorldVal, BottomPoints[IdxI]));
        RenderDrawDebugCube(TopPoints[IdxI], Sz, false, IsPointSolid(WorldVal, TopPoints[IdxI]));
        RenderDrawDebugCube(ShinPoints[IdxI], Wz, true, IsPointSolid(WorldVal, ShinPoints[IdxI]));
        RenderDrawDebugCube(FacePoints[IdxI], Wz, true, IsPointSolid(WorldVal, FacePoints[IdxI]));
    }
}
