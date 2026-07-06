#include "render/renderer.h"
#include "render/rl_compat.h"
#include "world/block_system.h"
#include "world/chunk.h"
#include "ui/debug.h"
#include "raylib.h"
#include "raymath.h"
#include "player/player.h"
#include <stddef.h>

// Must include glad before rlgl to get raw GL constants (GL_TEXTURE_2D_ARRAY etc.)
#include "external/glad.h"
#include <rlgl.h>

#define ATLAS_COLS 16.0F
#define ATLAS_ROWS 16.0F

#define VERTICES_PER_FACE 4
#define FLOATS_PER_VERTEX 3
#define COLOR_CHANNELS 4
#define OPAQUE_ALPHA_VALUE 255
#define CHUNK_CLOSE_DISTANCE_FACTOR 2.0F
#define INDICES_PER_FACES 6
#define MAX_FACES (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * INDICES_PER_FACES)

#define BLOCK_HIGHLIGHT_SCALE (BLOCK_SIZE + 0.01F)
#define CHUNK_SPHERE_RADIUS 14.0F

typedef struct {
  Texture2D BlockAtlas;
  unsigned int BlockArrayTexID;
  Shader ChunkShader;

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
} RendererState;

static RendererState *GetRendererState(void) {
  static RendererState State;
  return &State;
}

// ---------------------------------------------------------------------------
// Texture array loader
// ---------------------------------------------------------------------------

static void LoadBlockTextureArray(void) {
    RendererState *State = GetRendererState();
    Image AtlasImg = LoadImage("assets/atlas/terrain.png");
    ImageFormat(&AtlasImg, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    int TileW = AtlasImg.width  / (int)ATLAS_COLS;
    int TileH = AtlasImg.height / (int)ATLAS_ROWS;
    int TotalTiles = (int)ATLAS_COLS * (int)ATLAS_ROWS;

    glGenTextures(1, &State->BlockArrayTexID);
    glBindTexture(GL_TEXTURE_2D_ARRAY, State->BlockArrayTexID);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, TileW, TileH, TotalTiles,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void *)0);

    unsigned char *Pixels = (unsigned char *)AtlasImg.data;
    unsigned char *TileBuf = (unsigned char *)MemAlloc((size_t)(TileW * TileH * 4));

    #pragma unroll 4
    for (int TileIdx = 0; TileIdx < TotalTiles; TileIdx++) {
        int TileCol = TileIdx % (int)ATLAS_COLS;
        int TileRow = TileIdx / (int)ATLAS_COLS;

        #pragma unroll 4
        for (int Row = 0; Row < TileH; Row++) {
            int SrcRow = (TileRow * TileH) + Row;
            int SrcCol = TileCol * TileW;
            size_t DestOffset = (size_t)Row * (size_t)TileW * 4U;
            size_t SrcOffset = (((size_t)SrcRow * (size_t)AtlasImg.width) + (size_t)SrcCol) * 4U;
            size_t Len = (size_t)TileW * 4U;
            #pragma unroll 4
            for (size_t IdxI = 0; IdxI < Len; IdxI++) {
                TileBuf[DestOffset + IdxI] = Pixels[SrcOffset + IdxI];
            }
        }

        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, TileIdx,
                        TileW, TileH, 1, GL_RGBA, GL_UNSIGNED_BYTE, TileBuf);
    }

    MemFree(TileBuf);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    UnloadImage(AtlasImg);
}

void InitRenderer(void) {
    RendererState *State = GetRendererState();
    // Keep 2D atlas for DrawBlockIcon
    State->BlockAtlas = LoadTexture("assets/atlas/terrain.png");
    SetTextureFilter(State->BlockAtlas, TEXTURE_FILTER_POINT);

    LoadBlockTextureArray();

    State->ChunkShader = LoadShader("assets/shaders/chunk.vert", "assets/shaders/chunk.frag");
    int BlockArrayLoc = GetShaderLocation(State->ChunkShader, "uBlockArray");
    int Unit = 0;
    SetShaderValue(State->ChunkShader, BlockArrayLoc, &Unit, SHADER_UNIFORM_INT);
}

void CloseRenderer(void) {
    RendererState *State = GetRendererState();
    UnloadShader(State->ChunkShader);
    UnloadTexture(State->BlockAtlas);
    glDeleteTextures(1, &State->BlockArrayTexID);
}

// ---------------------------------------------------------------------------
// Atlas UV (DrawBlockIcon only)
// ---------------------------------------------------------------------------

static void GetTextureUV(int TextureIndex, Vector2 *UvMin, Vector2 *UvMax) {
    float U = (float)(TextureIndex % (int)ATLAS_COLS) / ATLAS_COLS;
    int Row = TextureIndex / (int)ATLAS_COLS;
    float V = (float)Row / ATLAS_ROWS;
    float W = 1.0F / ATLAS_COLS;
    float H = 1.0F / ATLAS_ROWS;
    UvMin->x = U; UvMin->y = V;
    UvMax->x = U + W; UvMax->y = V + H;
}

// ---------------------------------------------------------------------------
// AO helpers (unchanged)
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
    RendererState *State = GetRendererState();
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
    RendererState *State = GetRendererState();
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
    RendererState *State = GetRendererState();
    int UvIdx = (IsTrans ? State->TransVCount : State->VCount) * 2;
    float *UvArray = IsTrans ? State->TransTexCoords : State->TempTexCoords;
    UvArray[UvIdx++] = 0.0F; UvArray[UvIdx++] = VMax;
    UvArray[UvIdx++] = UMax; UvArray[UvIdx++] = VMax;
    UvArray[UvIdx++] = UMax; UvArray[UvIdx++] = 0.0F;
    UvArray[UvIdx++] = 0.0F; UvArray[UvIdx++] = 0.0F;
}

static void AddFaceTexLayer(float Layer, bool IsTrans) {
    RendererState *State = GetRendererState();
    int Tc2Idx = (IsTrans ? State->TransVCount : State->VCount) * 2;
    float *Tc2Array = IsTrans ? State->TransTexCoords2 : State->TempTexCoords2;
    #pragma unroll 4
    for (int IdxI = 0; IdxI < 4; IdxI++) {
        Tc2Array[Tc2Idx + (IdxI * 2) + 0] = Layer;
        Tc2Array[Tc2Idx + (IdxI * 2) + 1] = 0.0F;
    }
}

// Greedy quad vertices for a W×H merged face.
// Wx0,Wy0,Wz0 = world block coords of the first block in the merged region.
// W = width in u-axis blocks, H = height in v-axis blocks.
static void AddGreedyFaceVertices(BlockFace Face,
                                   int Wx0, int Wy0, int Wz0,
                                   int W, int H, bool IsTrans) {
    RendererState *State = GetRendererState();
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
    RendererState *State = GetRendererState();
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

void BuildChunkMesh(World *WorldVal, Chunk *ChunkVal) {
    RendererState *State = GetRendererState();
    if (ChunkVal->SolidBlockCount == 0) {
        UnloadChunkMesh(ChunkVal);
        ChunkVal->IsDirty = false;
        ChunkVal->HasMesh = false;
        ChunkVal->HasTranslucentMesh = false;
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
        ChunkVal->ChunkMesh = (Mesh){0};
        ChunkVal->ChunkMesh.vertexCount = State->VCount;
        ChunkVal->ChunkMesh.triangleCount = State->ICount / 3;

        ChunkVal->ChunkMesh.vertices = (float *)MemAlloc((size_t)State->VCount * FLOATS_PER_VERTEX * sizeof(float));
        size_t LenVerts = (size_t)State->VCount * FLOATS_PER_VERTEX;
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenVerts; IdxI++) {
            ChunkVal->ChunkMesh.vertices[IdxI] = State->TempVertices[IdxI];
        }

        ChunkVal->ChunkMesh.indices = (unsigned short *)MemAlloc((size_t)State->ICount * sizeof(unsigned short));
        size_t LenIndices = (size_t)State->ICount;
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenIndices; IdxI++) {
            ChunkVal->ChunkMesh.indices[IdxI] = State->TempIndices[IdxI];
        }

        ChunkVal->ChunkMesh.texcoords = (float *)MemAlloc((size_t)State->VCount * 2 * sizeof(float));
        size_t LenTex = (size_t)State->VCount * 2U;
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenTex; IdxI++) {
            ChunkVal->ChunkMesh.texcoords[IdxI] = State->TempTexCoords[IdxI];
        }

        ChunkVal->ChunkMesh.colors = (unsigned char *)MemAlloc((size_t)State->VCount * COLOR_CHANNELS * sizeof(unsigned char));
        size_t LenColors = (size_t)State->VCount * COLOR_CHANNELS;
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenColors; IdxI++) {
            ChunkVal->ChunkMesh.colors[IdxI] = State->TempColors[IdxI];
        }

        ChunkVal->ChunkMesh.texcoords2 = (float *)MemAlloc((size_t)State->VCount * 2 * sizeof(float));
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenTex; IdxI++) {
            ChunkVal->ChunkMesh.texcoords2[IdxI] = State->TempTexCoords2[IdxI];
        }

        UploadMesh(&ChunkVal->ChunkMesh, false);
        ChunkVal->HasMesh = true;
    }

    // Upload translucent mesh
    if (State->TransVCount == 0) {
        ChunkVal->HasTranslucentMesh = false;
    } else {
        ChunkVal->TranslucentMesh = (Mesh){0};
        ChunkVal->TranslucentMesh.vertexCount = State->TransVCount;
        ChunkVal->TranslucentMesh.triangleCount = State->TransICount / 3;

        ChunkVal->TranslucentMesh.vertices = (float *)MemAlloc((size_t)State->TransVCount * FLOATS_PER_VERTEX * sizeof(float));
        size_t LenVerts = (size_t)State->TransVCount * FLOATS_PER_VERTEX;
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenVerts; IdxI++) {
            ChunkVal->TranslucentMesh.vertices[IdxI] = State->TransVertices[IdxI];
        }

        ChunkVal->TranslucentMesh.indices = (unsigned short *)MemAlloc((size_t)State->TransICount * sizeof(unsigned short));
        size_t LenIndices = (size_t)State->TransICount;
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenIndices; IdxI++) {
            ChunkVal->TranslucentMesh.indices[IdxI] = State->TransIndices[IdxI];
        }

        ChunkVal->TranslucentMesh.texcoords = (float *)MemAlloc((size_t)State->TransVCount * 2 * sizeof(float));
        size_t LenTex = (size_t)State->TransVCount * 2U;
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenTex; IdxI++) {
            ChunkVal->TranslucentMesh.texcoords[IdxI] = State->TransTexCoords[IdxI];
        }

        ChunkVal->TranslucentMesh.colors = (unsigned char *)MemAlloc((size_t)State->TransVCount * COLOR_CHANNELS * sizeof(unsigned char));
        size_t LenColors = (size_t)State->TransVCount * COLOR_CHANNELS;
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenColors; IdxI++) {
            ChunkVal->TranslucentMesh.colors[IdxI] = State->TransColors[IdxI];
        }

        ChunkVal->TranslucentMesh.texcoords2 = (float *)MemAlloc((size_t)State->TransVCount * 2 * sizeof(float));
        #pragma unroll 4
        for (size_t IdxI = 0; IdxI < LenTex; IdxI++) {
            ChunkVal->TranslucentMesh.texcoords2[IdxI] = State->TransTexCoords2[IdxI];
        }

        UploadMesh(&ChunkVal->TranslucentMesh, false);
        ChunkVal->HasTranslucentMesh = true;
    }
}

// ---------------------------------------------------------------------------
// Custom draw — bypasses Raylib material/texture binding
// ---------------------------------------------------------------------------

static void DrawChunkMeshDirect(Mesh *MeshVal) {
    RendererState *State = GetRendererState();
    if (MeshVal->vaoId == 0) { return; }

    rlEnableShader(State->ChunkShader.id);

    Matrix MatView = rlGetMatrixModelview();
    Matrix MatProj = rlGetMatrixProjection();
    Matrix MatMVP  = MatrixMultiply(MatView, MatProj);
    rlSetUniformMatrix(State->ChunkShader.locs[SHADER_LOC_MATRIX_MVP], MatMVP);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, State->BlockArrayTexID);

    rlEnableVertexArray(MeshVal->vaoId);
    rlDrawVertexArrayElements(0, MeshVal->triangleCount * 3, 0);
    rlDisableVertexArray();

    rlDisableShader();
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void RenderChunkMesh(Chunk *ChunkVal) {
    if (ChunkVal->HasMesh) { DrawChunkMeshDirect(&ChunkVal->ChunkMesh); }
}

void RenderChunkTranslucentMesh(Chunk *ChunkVal) {
    if (ChunkVal->HasTranslucentMesh) { DrawChunkMeshDirect(&ChunkVal->TranslucentMesh); }
}

void UnloadChunkMesh(Chunk *ChunkVal) {
    if (ChunkVal->HasMesh) {
        UnloadMesh(ChunkVal->ChunkMesh);
        ChunkVal->HasMesh = false;
    }
    if (ChunkVal->HasTranslucentMesh) {
        UnloadMesh(ChunkVal->TranslucentMesh);
        ChunkVal->HasTranslucentMesh = false;
    }
}

// ---------------------------------------------------------------------------
// Frustum culling + world draw (unchanged logic)
// ---------------------------------------------------------------------------

bool IsChunkInFrustum(Camera3D CameraVal, Chunk *ChunkVal) {
    float const HALFCHUNKSIZE = CHUNK_SIZE / 2.0F;
    Vector3 ChunkCenter = {
        (float)(ChunkVal->ChunkX * CHUNK_SIZE) + HALFCHUNKSIZE,
        (float)(ChunkVal->ChunkY * CHUNK_SIZE) + HALFCHUNKSIZE,
        (float)(ChunkVal->ChunkZ * CHUNK_SIZE) + HALFCHUNKSIZE,
    };
    Vector3 VecToChunk = Vector3Subtract(ChunkCenter, CameraVal.position);
    float Distance = Vector3Length(VecToChunk);
    if (Distance < CHUNK_SPHERE_RADIUS * CHUNK_CLOSE_DISTANCE_FACTOR) { return true; }
    Vector3 DirToChunk = Vector3Scale(VecToChunk, 1.0F / Distance);
    Vector3 CamForward = Vector3Normalize(Vector3Subtract(CameraVal.target, CameraVal.position));
    float DotProduct = Vector3DotProduct(CamForward, DirToChunk);
    float SafeMargin = CHUNK_SPHERE_RADIUS / Distance;
#define FRUSTUM_DOT_THRESHOLD 0.4F
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

void DrawWorld(World *WorldVal, Camera3D CameraVal) {
    if (GetDebugState()->Wireframe) { rlEnableWireMode(); }

    // Opaque pass
    #pragma unroll 4
    for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
        if (!IsChunkInFrustum(CameraVal, &WorldVal->Chunks[IdxI])) { continue; }
        RenderChunkMesh(&WorldVal->Chunks[IdxI]);
        if (GetDebugState()->ChunkBorders) {
            Vector3 Center = {
                (float)(WorldVal->Chunks[IdxI].ChunkX * CHUNK_SIZE) + CHUNK_HALF_SIZE,
                (float)(WorldVal->Chunks[IdxI].ChunkY * CHUNK_SIZE) + CHUNK_HALF_SIZE,
                (float)(WorldVal->Chunks[IdxI].ChunkZ * CHUNK_SIZE) + CHUNK_HALF_SIZE,
            };
            DrawCubeWires(Center, CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE, YELLOW);
        }
    }

    // Translucent pass (back-to-front)
    rlDrawRenderBatchActive();
    rlDisableDepthMask();
    rlEnableColorBlend();

    static ChunkDistance VisibleChunks[MAX_ACTIVE_CHUNKS];
    int VisibleCount = 0;

    #pragma unroll 4
    for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
        Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
        if (!ChunkVal->HasTranslucentMesh) { continue; }
        if (!IsChunkInFrustum(CameraVal, ChunkVal)) { continue; }
        float const H = CHUNK_SIZE / 2.0F;
        Vector3 Center = {
            (float)(ChunkVal->ChunkX * CHUNK_SIZE) + H,
            (float)(ChunkVal->ChunkY * CHUNK_SIZE) + H,
            (float)(ChunkVal->ChunkZ * CHUNK_SIZE) + H,
        };
        Vector3 V = Vector3Subtract(Center, CameraVal.position);
        VisibleChunks[VisibleCount].Index = IdxI;
        VisibleChunks[VisibleCount].DistanceSq = (V.x*V.x) + (V.y*V.y) + (V.z*V.z);
        VisibleCount++;
    }

    ShellSortChunks(VisibleChunks, VisibleCount);
    #pragma unroll 4
    for (int IdxI = 0; IdxI < VisibleCount; IdxI++) {
        RenderChunkTranslucentMesh(&WorldVal->Chunks[VisibleChunks[IdxI].Index]);
    }

    rlDrawRenderBatchActive();
    rlEnableDepthMask();

    if (GetDebugState()->Wireframe) { rlDisableWireMode(); }
}

void DrawBlockHighlight(Vec3 Pos) {
    DrawCubeWires(Vec3ToRL(Pos), BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE, BLACK);
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
        DrawCube(Vec3ToRL(BottomPoints[IdxI]), Sz, Sz, Sz, IsPointSolid(WorldVal, BottomPoints[IdxI]) ? BLUE : RED);
        DrawCube(Vec3ToRL(TopPoints[IdxI]), Sz, Sz, Sz, IsPointSolid(WorldVal, TopPoints[IdxI]) ? BLUE : RED);
        DrawCubeWires(Vec3ToRL(ShinPoints[IdxI]), Wz, Wz, Wz, IsPointSolid(WorldVal, ShinPoints[IdxI]) ? PURPLE : ORANGE);
        DrawCubeWires(Vec3ToRL(FacePoints[IdxI]), Wz, Wz, Wz, IsPointSolid(WorldVal, FacePoints[IdxI]) ? PURPLE : ORANGE);
    }
}

#define BLOCK_ICON_HALF_DIVISOR  2.0F
#define BLOCK_ICON_QUART_DIVISOR 4.0F
#define BLOCK_ICON_COLOR_TOP     255U
#define BLOCK_ICON_COLOR_LEFT    150U
#define BLOCK_ICON_COLOR_RIGHT   200U
#define BLOCK_ICON_ALPHA         255U

void DrawBlockIcon(int BlockId, int X, int Y, int Size) {
    RendererState *State = GetRendererState();
    if (BlockId == 0) { return; }
    BlockType *Def = GetBlockDef(BlockId);

    Vector2 TopMin;
    Vector2 TopMax;
    Vector2 SideMin;
    Vector2 SideMax;
    GetTextureUV(Def->TexTop,  &TopMin,  &TopMax);
    GetTextureUV(Def->TexSide, &SideMin, &SideMax);

    float Cx = (float)X + ((float)Size / BLOCK_ICON_HALF_DIVISOR);
    float Cy = (float)Y + ((float)Size / BLOCK_ICON_HALF_DIVISOR);
    float H  = (float)Size / BLOCK_ICON_QUART_DIVISOR;

    Vector2 PTop        = {Cx, (float)Y};
    Vector2 PTopLeft    = {(float)X, (float)Y + H};
    Vector2 PTopRight   = {(float)(X + Size), (float)Y + H};
    Vector2 PCenter     = {Cx, Cy};
    Vector2 PBottomLeft = {(float)X, Cy + H};
    Vector2 PBottomRight = {(float)(X + Size), Cy + H};
    Vector2 PBottom     = {Cx, (float)(Y + Size)};

    rlSetTexture(State->BlockAtlas.id);
    rlBegin(RL_QUADS);

    rlColor4ub(BLOCK_ICON_COLOR_TOP, BLOCK_ICON_COLOR_TOP, BLOCK_ICON_COLOR_TOP, BLOCK_ICON_ALPHA);
    rlTexCoord2f(TopMin.x, TopMin.y); rlVertex2f(PTop.x,      PTop.y);
    rlTexCoord2f(TopMin.x, TopMax.y); rlVertex2f(PTopLeft.x,  PTopLeft.y);
    rlTexCoord2f(TopMax.x, TopMax.y); rlVertex2f(PCenter.x,   PCenter.y);
    rlTexCoord2f(TopMax.x, TopMin.y); rlVertex2f(PTopRight.x, PTopRight.y);

    rlColor4ub(BLOCK_ICON_COLOR_LEFT, BLOCK_ICON_COLOR_LEFT, BLOCK_ICON_COLOR_LEFT, BLOCK_ICON_ALPHA);
    rlTexCoord2f(SideMin.x, SideMin.y); rlVertex2f(PTopLeft.x,    PTopLeft.y);
    rlTexCoord2f(SideMin.x, SideMax.y); rlVertex2f(PBottomLeft.x, PBottomLeft.y);
    rlTexCoord2f(SideMax.x, SideMax.y); rlVertex2f(PBottom.x,     PBottom.y);
    rlTexCoord2f(SideMax.x, SideMin.y); rlVertex2f(PCenter.x,     PCenter.y);

    rlColor4ub(BLOCK_ICON_COLOR_RIGHT, BLOCK_ICON_COLOR_RIGHT, BLOCK_ICON_COLOR_RIGHT, BLOCK_ICON_ALPHA);
    rlTexCoord2f(SideMin.x, SideMin.y); rlVertex2f(PCenter.x,      PCenter.y);
    rlTexCoord2f(SideMin.x, SideMax.y); rlVertex2f(PBottom.x,      PBottom.y);
    rlTexCoord2f(SideMax.x, SideMax.y); rlVertex2f(PBottomRight.x, PBottomRight.y);
    rlTexCoord2f(SideMax.x, SideMin.y); rlVertex2f(PTopRight.x,    PTopRight.y);

    rlEnd();
    rlSetTexture(0);
}
