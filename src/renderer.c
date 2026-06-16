#include "renderer.h"
#include "block_system.h"
#include "chunk.h"
#include "debug.h"
#include "raylib.h"
#include "raymath.h"
#include "player.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
// Must include glad before rlgl to get raw GL constants (GL_TEXTURE_2D_ARRAY etc.)
#include "external/glad.h"
#include <rlgl.h>

#define ATLAS_COLS 16.0F
#define ATLAS_ROWS 16.0F

#define VERTICES_PER_FACE 4
#define FLOATS_PER_VERTEX 3
#define COLOR_CHANNELS 4
#define INDICES_PER_FACES 6
#define MAX_FACES (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * INDICES_PER_FACES)

#define BLOCK_HIGHLIGHT_SCALE (BLOCK_SIZE + 0.01F)
#define CHUNK_SPHERE_RADIUS 14.0F

// Kept for DrawBlockIcon atlas UV lookup only
static Texture2D blockAtlas;

// GL_TEXTURE_2D_ARRAY used by chunk shader
static unsigned int blockArrayTexID;

static Shader chunkShader;

static float tempTexCoords[MAX_FACES * VERTICES_PER_FACE * 2];
static float tempVertices[MAX_FACES * VERTICES_PER_FACE * FLOATS_PER_VERTEX];
static unsigned short tempIndices[MAX_FACES * INDICES_PER_FACES];
static unsigned char tempColors[MAX_FACES * VERTICES_PER_FACE * COLOR_CHANNELS];
static float tempTexCoords2[MAX_FACES * VERTICES_PER_FACE * 2];

static int vCount = 0;
static int iCount = 0;

static float transTexCoords[MAX_FACES * VERTICES_PER_FACE * 2];
static float transVertices[MAX_FACES * VERTICES_PER_FACE * FLOATS_PER_VERTEX];
static unsigned short transIndices[MAX_FACES * INDICES_PER_FACES];
static unsigned char transColors[MAX_FACES * VERTICES_PER_FACE * COLOR_CHANNELS];
static float transTexCoords2[MAX_FACES * VERTICES_PER_FACE * 2];

static int transVCount = 0;
static int transICount = 0;

// ---------------------------------------------------------------------------
// Texture array loader
// ---------------------------------------------------------------------------

static void LoadBlockTextureArray(void) {
    Image atlasImg = LoadImage("assets/atlas/terrain.png");
    ImageFormat(&atlasImg, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    int tileW = atlasImg.width  / (int)ATLAS_COLS;
    int tileH = atlasImg.height / (int)ATLAS_ROWS;
    int totalTiles = (int)ATLAS_COLS * (int)ATLAS_ROWS;

    glGenTextures(1, &blockArrayTexID);
    glBindTexture(GL_TEXTURE_2D_ARRAY, blockArrayTexID);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, tileW, tileH, totalTiles,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    unsigned char *pixels = (unsigned char *)atlasImg.data;
    unsigned char *tileBuf = (unsigned char *)MemAlloc((size_t)(tileW * tileH * 4));

    for (int tileIdx = 0; tileIdx < totalTiles; tileIdx++) {
        int tileCol = tileIdx % (int)ATLAS_COLS;
        int tileRow = tileIdx / (int)ATLAS_COLS;

        for (int row = 0; row < tileH; row++) {
            int srcRow = tileRow * tileH + row;
            int srcCol = tileCol * tileW;
            memcpy(tileBuf + row * tileW * 4,
                   pixels + (srcRow * atlasImg.width + srcCol) * 4,
                   (size_t)(tileW * 4));
        }

        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, tileIdx,
                        tileW, tileH, 1, GL_RGBA, GL_UNSIGNED_BYTE, tileBuf);
    }

    MemFree(tileBuf);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    UnloadImage(atlasImg);
}

void InitRenderer(void) {
    // Keep 2D atlas for DrawBlockIcon
    blockAtlas = LoadTexture("assets/atlas/terrain.png");
    SetTextureFilter(blockAtlas, TEXTURE_FILTER_POINT);

    LoadBlockTextureArray();

    chunkShader = LoadShader("assets/shaders/chunk.vert", "assets/shaders/chunk.frag");
    int blockArrayLoc = GetShaderLocation(chunkShader, "uBlockArray");
    int unit = 0;
    SetShaderValue(chunkShader, blockArrayLoc, &unit, SHADER_UNIFORM_INT);
}

void CloseRenderer(void) {
    UnloadShader(chunkShader);
    UnloadTexture(blockAtlas);
    glDeleteTextures(1, &blockArrayTexID);
}

// ---------------------------------------------------------------------------
// Atlas UV (DrawBlockIcon only)
// ---------------------------------------------------------------------------

static void GetTextureUV(int textureIndex, Vector2 *uvMin, Vector2 *uvMax) {
    float u = (float)(textureIndex % (int)ATLAS_COLS) / ATLAS_COLS;
    float v = (float)(textureIndex / (int)ATLAS_COLS) / ATLAS_ROWS;
    float w = 1.0F / ATLAS_COLS;
    float h = 1.0F / ATLAS_ROWS;
    uvMin->x = u; uvMin->y = v;
    uvMax->x = u + w; uvMax->y = v + h;
}

// ---------------------------------------------------------------------------
// AO helpers (unchanged)
// ---------------------------------------------------------------------------

static unsigned char GetBlockIDAtLocal(World *world, Chunk *chunk, int lx, int ly, int lz) {
    if (lx >= 0 && lx < CHUNK_SIZE &&
        ly >= 0 && ly < CHUNK_SIZE &&
        lz >= 0 && lz < CHUNK_SIZE) {
        return chunk->data[lx][ly][lz];
    }
    int gx = chunk->chunkX * CHUNK_SIZE + lx;
    int gy = chunk->chunkY * CHUNK_SIZE + ly;
    int gz = chunk->chunkZ * CHUNK_SIZE + lz;
    return (unsigned char)GetBlockIDFromWorld(world, (Vector3){(float)gx, (float)gy, (float)gz});
}

static bool IsNeighbourTransparent(World *world, Chunk *chunk,
                                   int localX, int localY, int localZ,
                                   unsigned char selfID) {
    unsigned char id = GetBlockIDAtLocal(world, chunk, localX, localY, localZ);
    if (id == 0) return true;
    if (id == selfID && GetBlockDef(selfID)->isTransparent) return false;
    return GetBlockDef(id)->isTransparent;
}

static bool IsSolidForAO(World *world, Chunk *chunk, int localX, int localY, int localZ) {
    unsigned char id = GetBlockIDAtLocal(world, chunk, localX, localY, localZ);
    if (id == 0) return false;
    return GetBlockDef(id)->isSolid;
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

static bool ComputeFaceAO(World *world, Chunk *chunk,
                           int lx, int ly, int lz, BlockFace face, int ao[4]) {
    for (int v = 0; v < 4; v++) {
        int s1x = lx + AO_OFFSETS[face][v][0][0];
        int s1y = ly + AO_OFFSETS[face][v][0][1];
        int s1z = lz + AO_OFFSETS[face][v][0][2];
        int s2x = lx + AO_OFFSETS[face][v][1][0];
        int s2y = ly + AO_OFFSETS[face][v][1][1];
        int s2z = lz + AO_OFFSETS[face][v][1][2];
        int cx  = lx + AO_OFFSETS[face][v][2][0];
        int cy  = ly + AO_OFFSETS[face][v][2][1];
        int cz  = lz + AO_OFFSETS[face][v][2][2];

        bool s1 = IsSolidForAO(world, chunk, s1x, s1y, s1z);
        bool s2 = IsSolidForAO(world, chunk, s2x, s2y, s2z);
        bool corner = IsSolidForAO(world, chunk, cx, cy, cz);

        if (s1 && s2) ao[v] = 3;
        else ao[v] = (s1 ? 1 : 0) + (s2 ? 1 : 0) + (corner ? 1 : 0);
    }
    return (ao[0] + ao[2] > ao[1] + ao[3]);
}

// ---------------------------------------------------------------------------
// Mesh builder primitives
// ---------------------------------------------------------------------------

static void AddFaceIndices(bool isTrans, bool flipQuad) {
    if (isTrans) {
        if (flipQuad) {
            transIndices[transICount++] = transVCount + 0;
            transIndices[transICount++] = transVCount + 1;
            transIndices[transICount++] = transVCount + 3;
            transIndices[transICount++] = transVCount + 1;
            transIndices[transICount++] = transVCount + 2;
            transIndices[transICount++] = transVCount + 3;
        } else {
            transIndices[transICount++] = transVCount + 0;
            transIndices[transICount++] = transVCount + 1;
            transIndices[transICount++] = transVCount + 2;
            transIndices[transICount++] = transVCount + 0;
            transIndices[transICount++] = transVCount + 2;
            transIndices[transICount++] = transVCount + 3;
        }
    } else {
        if (flipQuad) {
            tempIndices[iCount++] = vCount + 0;
            tempIndices[iCount++] = vCount + 1;
            tempIndices[iCount++] = vCount + 3;
            tempIndices[iCount++] = vCount + 1;
            tempIndices[iCount++] = vCount + 2;
            tempIndices[iCount++] = vCount + 3;
        } else {
            tempIndices[iCount++] = vCount + 0;
            tempIndices[iCount++] = vCount + 1;
            tempIndices[iCount++] = vCount + 2;
            tempIndices[iCount++] = vCount + 0;
            tempIndices[iCount++] = vCount + 2;
            tempIndices[iCount++] = vCount + 3;
        }
    }
}

static void AddFaceColors(const int ao[4], bool isTrans) {
    int colIdx = (isTrans ? transVCount : vCount) * COLOR_CHANNELS;
    unsigned char *colorsArray = isTrans ? transColors : tempColors;
    for (int i = 0; i < 4; i++) {
        unsigned char b = AO_BRIGHTNESS[ao[i]];
        colorsArray[colIdx++] = b;
        colorsArray[colIdx++] = b;
        colorsArray[colIdx++] = b;
        colorsArray[colIdx++] = 255;
    }
}

// UV: raw tile-count coords [0..W, 0..H] — shader + array texture handles tiling
static void AddGreedyFaceTexCoords(float uMax, float vMax, bool isTrans) {
    int uvIdx = (isTrans ? transVCount : vCount) * 2;
    float *uvArray = isTrans ? transTexCoords : tempTexCoords;
    uvArray[uvIdx++] = 0.0f; uvArray[uvIdx++] = vMax;
    uvArray[uvIdx++] = uMax; uvArray[uvIdx++] = vMax;
    uvArray[uvIdx++] = uMax; uvArray[uvIdx++] = 0.0f;
    uvArray[uvIdx++] = 0.0f; uvArray[uvIdx++] = 0.0f;
}

static void AddFaceTexLayer(float layer, bool isTrans) {
    int tc2Idx = (isTrans ? transVCount : vCount) * 2;
    float *tc2Array = isTrans ? transTexCoords2 : tempTexCoords2;
    for (int i = 0; i < 4; i++) {
        tc2Array[tc2Idx + i * 2 + 0] = layer;
        tc2Array[tc2Idx + i * 2 + 1] = 0.0f;
    }
}

// Greedy quad vertices for a W×H merged face.
// wx0,wy0,wz0 = world block coords of the first block in the merged region.
// W = width in u-axis blocks, H = height in v-axis blocks.
static void AddGreedyFaceVertices(BlockFace face,
                                   int wx0, int wy0, int wz0,
                                   int W, int H, bool isTrans) {
    float *vArray = isTrans ? transVertices : tempVertices;
    int vIdx = (isTrans ? transVCount : vCount) * FLOATS_PER_VERTEX;

    float x0 = (float)wx0 - 0.5f;
    float y0 = (float)wy0 - 0.5f;
    float z0 = (float)wz0 - 0.5f;
    float fw = (float)W;
    float fh = (float)H;

    switch (face) {
        case FACE_TOP:    // u=X, v=Z
            vArray[vIdx++] = x0;    vArray[vIdx++] = y0+1; vArray[vIdx++] = z0+fh;
            vArray[vIdx++] = x0+fw; vArray[vIdx++] = y0+1; vArray[vIdx++] = z0+fh;
            vArray[vIdx++] = x0+fw; vArray[vIdx++] = y0+1; vArray[vIdx++] = z0;
            vArray[vIdx++] = x0;    vArray[vIdx++] = y0+1; vArray[vIdx++] = z0;
            break;
        case FACE_BOTTOM: // u=X, v=Z (winding reversed)
            vArray[vIdx++] = x0;    vArray[vIdx++] = y0; vArray[vIdx++] = z0;
            vArray[vIdx++] = x0+fw; vArray[vIdx++] = y0; vArray[vIdx++] = z0;
            vArray[vIdx++] = x0+fw; vArray[vIdx++] = y0; vArray[vIdx++] = z0+fh;
            vArray[vIdx++] = x0;    vArray[vIdx++] = y0; vArray[vIdx++] = z0+fh;
            break;
        case FACE_FRONT:  // u=X, v=Y
            vArray[vIdx++] = x0;    vArray[vIdx++] = y0;    vArray[vIdx++] = z0+1;
            vArray[vIdx++] = x0+fw; vArray[vIdx++] = y0;    vArray[vIdx++] = z0+1;
            vArray[vIdx++] = x0+fw; vArray[vIdx++] = y0+fh; vArray[vIdx++] = z0+1;
            vArray[vIdx++] = x0;    vArray[vIdx++] = y0+fh; vArray[vIdx++] = z0+1;
            break;
        case FACE_BACK:   // u=X reversed, v=Y
            vArray[vIdx++] = x0+fw; vArray[vIdx++] = y0;    vArray[vIdx++] = z0;
            vArray[vIdx++] = x0;    vArray[vIdx++] = y0;    vArray[vIdx++] = z0;
            vArray[vIdx++] = x0;    vArray[vIdx++] = y0+fh; vArray[vIdx++] = z0;
            vArray[vIdx++] = x0+fw; vArray[vIdx++] = y0+fh; vArray[vIdx++] = z0;
            break;
        case FACE_LEFT:   // u=Z, v=Y
            vArray[vIdx++] = x0; vArray[vIdx++] = y0;    vArray[vIdx++] = z0;
            vArray[vIdx++] = x0; vArray[vIdx++] = y0;    vArray[vIdx++] = z0+fw;
            vArray[vIdx++] = x0; vArray[vIdx++] = y0+fh; vArray[vIdx++] = z0+fw;
            vArray[vIdx++] = x0; vArray[vIdx++] = y0+fh; vArray[vIdx++] = z0;
            break;
        case FACE_RIGHT:  // u=Z reversed, v=Y
            vArray[vIdx++] = x0+1; vArray[vIdx++] = y0;    vArray[vIdx++] = z0+fw;
            vArray[vIdx++] = x0+1; vArray[vIdx++] = y0;    vArray[vIdx++] = z0;
            vArray[vIdx++] = x0+1; vArray[vIdx++] = y0+fh; vArray[vIdx++] = z0;
            vArray[vIdx++] = x0+1; vArray[vIdx++] = y0+fh; vArray[vIdx++] = z0+fw;
            break;
    }
}

static void AddGreedyFaceToMeshBuilder(BlockFace face,
                                        int wx0, int wy0, int wz0,
                                        int W, int H,
                                        int texIndex, const int ao[4],
                                        bool flipQuad, bool isTrans) {
    AddFaceIndices(isTrans, flipQuad);
    AddGreedyFaceTexCoords((float)W, (float)H, isTrans);
    AddFaceColors(ao, isTrans);
    AddFaceTexLayer((float)texIndex, isTrans);
    AddGreedyFaceVertices(face, wx0, wy0, wz0, W, H, isTrans);

    if (isTrans) transVCount += 4;
    else vCount += 4;
}

// ---------------------------------------------------------------------------
// FaceMask and greedy scan
// ---------------------------------------------------------------------------

typedef struct {
    unsigned char blockID;
    unsigned char texIndex;
    int ao[4];
    bool flipQuad;
    bool used;
} FaceMask;

typedef struct {
    BlockFace face;
    int normalAxis; // 0=X, 1=Y, 2=Z
    int normalDir;  // +1 or -1
    int uAxis;
    int vAxis;
} FaceDir;

static const FaceDir FACE_DIRS[6] = {
    { FACE_TOP,    1, +1, 0, 2 },
    { FACE_BOTTOM, 1, -1, 0, 2 },
    { FACE_RIGHT,  0, +1, 2, 1 },
    { FACE_LEFT,   0, -1, 2, 1 },
    { FACE_FRONT,  2, +1, 0, 1 },
    { FACE_BACK,   2, -1, 0, 1 },
};

static int GetFaceTex(BlockType *def, BlockFace face) {
    if (face == FACE_TOP) return def->texTop;
    if (face == FACE_BOTTOM) return def->texBottom;
    return def->texSide;
}

static bool MasksCompatible(const FaceMask *a, const FaceMask *b) {
    if (a->blockID == 0 || b->blockID == 0) return false;
    if (a->used || b->used) return false;
    if (a->blockID != b->blockID) return false;
    if (a->texIndex != b->texIndex) return false;
    if (a->flipQuad != b->flipQuad) return false;
    for (int i = 0; i < 4; i++) if (a->ao[i] != b->ao[i]) return false;
    return true;
}

static void BuildGreedyFacePass(World *world, Chunk *chunk, const FaceDir *fd) {
    for (int slice = 0; slice < CHUNK_SIZE; slice++) {
        FaceMask mask[CHUNK_SIZE][CHUNK_SIZE];

        // Build mask for this slice
        for (int u = 0; u < CHUNK_SIZE; u++) {
            for (int v = 0; v < CHUNK_SIZE; v++) {
                mask[u][v].blockID = 0;
                mask[u][v].used = false;

                int lpos[3];
                lpos[fd->normalAxis] = slice;
                lpos[fd->uAxis] = u;
                lpos[fd->vAxis] = v;
                int lx = lpos[0], ly = lpos[1], lz = lpos[2];

                unsigned char id = chunk->data[lx][ly][lz];
                if (id == 0) continue;

                BlockType *def = GetBlockDef(id);
                if (def->isTransparent) continue;

                int nlpos[3] = { lpos[0], lpos[1], lpos[2] };
                nlpos[fd->normalAxis] += fd->normalDir;
                if (!IsNeighbourTransparent(world, chunk,
                                            nlpos[0], nlpos[1], nlpos[2], id)) continue;

                mask[u][v].blockID   = id;
                mask[u][v].texIndex  = (unsigned char)GetFaceTex(def, fd->face);
                mask[u][v].flipQuad  = ComputeFaceAO(world, chunk, lx, ly, lz,
                                                     fd->face, mask[u][v].ao);
            }
        }

        // Greedy scan
        for (int u0 = 0; u0 < CHUNK_SIZE; u0++) {
            for (int v0 = 0; v0 < CHUNK_SIZE; v0++) {
                FaceMask *origin = &mask[u0][v0];
                if (origin->blockID == 0 || origin->used) continue;

                // Expand width (u direction)
                int w = 1;
                while (u0 + w < CHUNK_SIZE &&
                       MasksCompatible(origin, &mask[u0 + w][v0])) w++;

                // Expand height (v direction)
                int h = 1;
                bool can_expand = true;
                while (v0 + h < CHUNK_SIZE && can_expand) {
                    for (int ku = 0; ku < w; ku++) {
                        if (!MasksCompatible(origin, &mask[u0 + ku][v0 + h])) {
                            can_expand = false;
                            break;
                        }
                    }
                    if (can_expand) h++;
                }

                // Compute world position of first block in merged region
                int first_lpos[3];
                first_lpos[fd->normalAxis] = slice;
                first_lpos[fd->uAxis] = u0;
                first_lpos[fd->vAxis] = v0;
                int wx0 = chunk->chunkX * CHUNK_SIZE + first_lpos[0];
                int wy0 = chunk->chunkY * CHUNK_SIZE + first_lpos[1];
                int wz0 = chunk->chunkZ * CHUNK_SIZE + first_lpos[2];

                AddGreedyFaceToMeshBuilder(fd->face, wx0, wy0, wz0, w, h,
                                           origin->texIndex, origin->ao,
                                           origin->flipQuad, false);

                // Mark used
                for (int ku = 0; ku < w; ku++)
                    for (int kv = 0; kv < h; kv++)
                        mask[u0 + ku][v0 + kv].used = true;
            }
        }
    }
}

// Per-face pass for transparent blocks (no greedy merging)
static void BuildTransparentFacePass(World *world, Chunk *chunk) {
    for (int x = 0; x < CHUNK_SIZE; x++) {
        for (int y = 0; y < CHUNK_SIZE; y++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                unsigned char id = chunk->data[x][y][z];
                if (id == 0) continue;

                BlockType *def = GetBlockDef(id);
                if (!def->isTransparent) continue;

                int ao[4];
                bool flip;

                int wx = chunk->chunkX * CHUNK_SIZE + x;
                int wy = chunk->chunkY * CHUNK_SIZE + y;
                int wz = chunk->chunkZ * CHUNK_SIZE + z;

                if (IsNeighbourTransparent(world, chunk, x, y+1, z, id)) {
                    flip = ComputeFaceAO(world, chunk, x, y, z, FACE_TOP, ao);
                    AddGreedyFaceToMeshBuilder(FACE_TOP, wx, wy, wz, 1, 1,
                                               def->texTop, ao, flip, true);
                }
                if (IsNeighbourTransparent(world, chunk, x, y-1, z, id)) {
                    flip = ComputeFaceAO(world, chunk, x, y, z, FACE_BOTTOM, ao);
                    AddGreedyFaceToMeshBuilder(FACE_BOTTOM, wx, wy, wz, 1, 1,
                                               def->texBottom, ao, flip, true);
                }
                if (IsNeighbourTransparent(world, chunk, x+1, y, z, id)) {
                    flip = ComputeFaceAO(world, chunk, x, y, z, FACE_RIGHT, ao);
                    AddGreedyFaceToMeshBuilder(FACE_RIGHT, wx, wy, wz, 1, 1,
                                               def->texSide, ao, flip, true);
                }
                if (IsNeighbourTransparent(world, chunk, x-1, y, z, id)) {
                    flip = ComputeFaceAO(world, chunk, x, y, z, FACE_LEFT, ao);
                    AddGreedyFaceToMeshBuilder(FACE_LEFT, wx, wy, wz, 1, 1,
                                               def->texSide, ao, flip, true);
                }
                if (IsNeighbourTransparent(world, chunk, x, y, z+1, id)) {
                    flip = ComputeFaceAO(world, chunk, x, y, z, FACE_FRONT, ao);
                    AddGreedyFaceToMeshBuilder(FACE_FRONT, wx, wy, wz, 1, 1,
                                               def->texSide, ao, flip, true);
                }
                if (IsNeighbourTransparent(world, chunk, x, y, z-1, id)) {
                    flip = ComputeFaceAO(world, chunk, x, y, z, FACE_BACK, ao);
                    AddGreedyFaceToMeshBuilder(FACE_BACK, wx, wy, wz, 1, 1,
                                               def->texSide, ao, flip, true);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// BuildChunkMesh
// ---------------------------------------------------------------------------

void BuildChunkMesh(World *world, Chunk *chunk) {
    if (chunk->solidBlockCount == 0) {
        UnloadChunkMesh(chunk);
        chunk->isDirty = false;
        chunk->hasMesh = false;
        chunk->hasTranslucentMesh = false;
        return;
    }

    vCount = 0; iCount = 0;
    transVCount = 0; transICount = 0;

    // Opaque greedy pass
    for (int fd = 0; fd < 6; fd++) BuildGreedyFacePass(world, chunk, &FACE_DIRS[fd]);

    // Transparent per-face pass
    BuildTransparentFacePass(world, chunk);

    UnloadChunkMesh(chunk);
    chunk->isDirty = false;

    // Upload opaque mesh
    if (vCount == 0) {
        chunk->hasMesh = false;
    } else {
        chunk->mesh = (Mesh){0};
        chunk->mesh.vertexCount = vCount;
        chunk->mesh.triangleCount = iCount / 3;

        chunk->mesh.vertices = (float *)MemAlloc((size_t)vCount * FLOATS_PER_VERTEX * sizeof(float));
        memcpy(chunk->mesh.vertices, tempVertices, (size_t)vCount * FLOATS_PER_VERTEX * sizeof(float));

        chunk->mesh.indices = (unsigned short *)MemAlloc((size_t)iCount * sizeof(unsigned short));
        memcpy(chunk->mesh.indices, tempIndices, (size_t)iCount * sizeof(unsigned short));

        chunk->mesh.texcoords = (float *)MemAlloc((size_t)vCount * 2 * sizeof(float));
        memcpy(chunk->mesh.texcoords, tempTexCoords, (size_t)vCount * 2 * sizeof(float));

        chunk->mesh.colors = (unsigned char *)MemAlloc((size_t)vCount * COLOR_CHANNELS * sizeof(unsigned char));
        memcpy(chunk->mesh.colors, tempColors, (size_t)vCount * COLOR_CHANNELS * sizeof(unsigned char));

        chunk->mesh.texcoords2 = (float *)MemAlloc((size_t)vCount * 2 * sizeof(float));
        memcpy(chunk->mesh.texcoords2, tempTexCoords2, (size_t)vCount * 2 * sizeof(float));

        UploadMesh(&chunk->mesh, false);
        chunk->hasMesh = true;
    }

    // Upload translucent mesh
    if (transVCount == 0) {
        chunk->hasTranslucentMesh = false;
    } else {
        chunk->translucentMesh = (Mesh){0};
        chunk->translucentMesh.vertexCount = transVCount;
        chunk->translucentMesh.triangleCount = transICount / 3;

        chunk->translucentMesh.vertices = (float *)MemAlloc((size_t)transVCount * FLOATS_PER_VERTEX * sizeof(float));
        memcpy(chunk->translucentMesh.vertices, transVertices, (size_t)transVCount * FLOATS_PER_VERTEX * sizeof(float));

        chunk->translucentMesh.indices = (unsigned short *)MemAlloc((size_t)transICount * sizeof(unsigned short));
        memcpy(chunk->translucentMesh.indices, transIndices, (size_t)transICount * sizeof(unsigned short));

        chunk->translucentMesh.texcoords = (float *)MemAlloc((size_t)transVCount * 2 * sizeof(float));
        memcpy(chunk->translucentMesh.texcoords, transTexCoords, (size_t)transVCount * 2 * sizeof(float));

        chunk->translucentMesh.colors = (unsigned char *)MemAlloc((size_t)transVCount * COLOR_CHANNELS * sizeof(unsigned char));
        memcpy(chunk->translucentMesh.colors, transColors, (size_t)transVCount * COLOR_CHANNELS * sizeof(unsigned char));

        chunk->translucentMesh.texcoords2 = (float *)MemAlloc((size_t)transVCount * 2 * sizeof(float));
        memcpy(chunk->translucentMesh.texcoords2, transTexCoords2, (size_t)transVCount * 2 * sizeof(float));

        UploadMesh(&chunk->translucentMesh, false);
        chunk->hasTranslucentMesh = true;
    }
}

// ---------------------------------------------------------------------------
// Custom draw — bypasses Raylib material/texture binding
// ---------------------------------------------------------------------------

static void DrawChunkMeshDirect(Mesh *mesh) {
    if (mesh->vaoId == 0) return;

    rlEnableShader(chunkShader.id);

    Matrix matView = rlGetMatrixModelview();
    Matrix matProj = rlGetMatrixProjection();
    Matrix matMVP  = MatrixMultiply(matView, matProj);
    rlSetUniformMatrix(chunkShader.locs[SHADER_LOC_MATRIX_MVP], matMVP);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, blockArrayTexID);

    rlEnableVertexArray(mesh->vaoId);
    rlDrawVertexArrayElements(0, mesh->triangleCount * 3, 0);
    rlDisableVertexArray();

    rlDisableShader();
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void RenderChunkMesh(Chunk *chunk) {
    if (chunk->hasMesh) DrawChunkMeshDirect(&chunk->mesh);
}

void RenderChunkTranslucentMesh(Chunk *chunk) {
    if (chunk->hasTranslucentMesh) DrawChunkMeshDirect(&chunk->translucentMesh);
}

void UnloadChunkMesh(Chunk *chunk) {
    if (chunk->hasMesh) {
        UnloadMesh(chunk->mesh);
        chunk->hasMesh = false;
    }
    if (chunk->hasTranslucentMesh) {
        UnloadMesh(chunk->translucentMesh);
        chunk->hasTranslucentMesh = false;
    }
}

// ---------------------------------------------------------------------------
// Frustum culling + world draw (unchanged logic)
// ---------------------------------------------------------------------------

bool IsChunkInFrustum(Camera3D camera, Chunk *chunk) {
    float const HALFCHUNKSIZE = CHUNK_SIZE / 2.0F;
    Vector3 chunkCenter = {
        (float)(chunk->chunkX * CHUNK_SIZE) + HALFCHUNKSIZE,
        (float)(chunk->chunkY * CHUNK_SIZE) + HALFCHUNKSIZE,
        (float)(chunk->chunkZ * CHUNK_SIZE) + HALFCHUNKSIZE,
    };
    Vector3 vecToChunk = Vector3Subtract(chunkCenter, camera.position);
    float distance = Vector3Length(vecToChunk);
    if (distance < CHUNK_SPHERE_RADIUS * 2.0F) return true;
    Vector3 dirToChunk = Vector3Scale(vecToChunk, 1.0F / distance);
    Vector3 camForward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    float dotProduct = Vector3DotProduct(camForward, dirToChunk);
    float safeMargin = CHUNK_SPHERE_RADIUS / distance;
    return dotProduct >= (0.4f - safeMargin);
}

typedef struct {
    int index;
    float distanceSq;
} ChunkDistance;

static int CompareChunkDistance(const void *a, const void *b) {
    float dA = ((ChunkDistance *)a)->distanceSq;
    float dB = ((ChunkDistance *)b)->distanceSq;
    return (dB > dA) - (dB < dA);
}

void DrawWorld(World *world, Camera3D camera) {
    if (g_debug.wireframe) rlEnableWireMode();

    // Opaque pass
    for (int i = 0; i < world->chunkCount; i++) {
        if (!IsChunkInFrustum(camera, &world->chunks[i])) continue;
        RenderChunkMesh(&world->chunks[i]);
        if (g_debug.chunkBorders) {
            Vector3 center = {
                (float)(world->chunks[i].chunkX * CHUNK_SIZE) + CHUNK_HALF_SIZE,
                (float)(world->chunks[i].chunkY * CHUNK_SIZE) + CHUNK_HALF_SIZE,
                (float)(world->chunks[i].chunkZ * CHUNK_SIZE) + CHUNK_HALF_SIZE,
            };
            DrawCubeWires(center, CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE, YELLOW);
        }
    }

    // Translucent pass (back-to-front)
    rlDrawRenderBatchActive();
    rlDisableDepthMask();
    rlEnableColorBlend();

    static ChunkDistance visibleChunks[MAX_ACTIVE_CHUNKS];
    int visibleCount = 0;

    for (int i = 0; i < world->chunkCount; i++) {
        Chunk *c = &world->chunks[i];
        if (!c->hasTranslucentMesh) continue;
        if (!IsChunkInFrustum(camera, c)) continue;
        float const H = CHUNK_SIZE / 2.0F;
        Vector3 center = {
            (float)(c->chunkX * CHUNK_SIZE) + H,
            (float)(c->chunkY * CHUNK_SIZE) + H,
            (float)(c->chunkZ * CHUNK_SIZE) + H,
        };
        Vector3 v = Vector3Subtract(center, camera.position);
        visibleChunks[visibleCount].index = i;
        visibleChunks[visibleCount].distanceSq = v.x*v.x + v.y*v.y + v.z*v.z;
        visibleCount++;
    }

    qsort(visibleChunks, visibleCount, sizeof(ChunkDistance), CompareChunkDistance);
    for (int i = 0; i < visibleCount; i++)
        RenderChunkTranslucentMesh(&world->chunks[visibleChunks[i].index]);

    rlDrawRenderBatchActive();
    rlEnableDepthMask();

    if (g_debug.wireframe) rlDisableWireMode();
}

void DrawBlockHighlight(Vector3 pos) {
    DrawCubeWires(pos, BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE, BLACK);
}

void DrawAABBDebug(World *world, Player *player) {
    if (!g_debug.AABB) return;

    Vector3 bottomPoints[COLLISION_POINTS];
    Vector3 topPoints[COLLISION_POINTS];
    Vector3 shinPoints[COLLISION_POINTS];
    Vector3 facePoints[COLLISION_POINTS];

    GetPlayerPoints(player, (PointConfig){ .radius = player->radius - VERTICAL_RADIUS_SHRINK, .yOffset = 0.0F, .epsilon = -COLLISION_EPSILON }, bottomPoints);
    GetPlayerPoints(player, (PointConfig){ .radius = player->radius - VERTICAL_RADIUS_SHRINK, .yOffset = player->size.y, .epsilon = COLLISION_EPSILON }, topPoints);

    const float LATERALDEBUGRADIUS = player->radius + 0.01F;
    GetPlayerPoints(player, (PointConfig){ .radius = LATERALDEBUGRADIUS, .yOffset = LATERAL_Y_MARGIN, .epsilon = 0.0F }, shinPoints);
    GetPlayerPoints(player, (PointConfig){ .radius = LATERALDEBUGRADIUS, .yOffset = player->size.y, .epsilon = 0.0F }, facePoints);

    float sz = PLAYER_DEBUG_AABB_SQUARES_SIZE;
    float wz = PLAYER_DEBUG_AABB_WIRES_SIZE;

    for (int i = 0; i < COLLISION_POINTS; i++) {
        DrawCube(bottomPoints[i], sz, sz, sz, IsPointSolid(world, bottomPoints[i]) ? BLUE : RED);
        DrawCube(topPoints[i], sz, sz, sz, IsPointSolid(world, topPoints[i]) ? BLUE : RED);
        DrawCubeWires(shinPoints[i], wz, wz, wz, IsPointSolid(world, shinPoints[i]) ? PURPLE : ORANGE);
        DrawCubeWires(facePoints[i], wz, wz, wz, IsPointSolid(world, facePoints[i]) ? PURPLE : ORANGE);
    }
}

void DrawBlockIcon(int blockID, int x, int y, int size) {
    if (blockID == 0) return;
    BlockType *def = GetBlockDef(blockID);

    Vector2 topMin, topMax, sideMin, sideMax;
    GetTextureUV(def->texTop,  &topMin,  &topMax);
    GetTextureUV(def->texSide, &sideMin, &sideMax);

    float cx = x + size / 2.0f;
    float cy = y + size / 2.0f;
    float h  = size / 4.0f;

    Vector2 pTop        = {cx, (float)y};
    Vector2 pTopLeft    = {(float)x, y + h};
    Vector2 pTopRight   = {(float)(x + size), y + h};
    Vector2 pCenter     = {cx, cy};
    Vector2 pBottomLeft = {(float)x, cy + h};
    Vector2 pBottomRight = {(float)(x + size), cy + h};
    Vector2 pBottom     = {cx, (float)(y + size)};

    rlSetTexture(blockAtlas.id);
    rlBegin(RL_QUADS);

    rlColor4ub(255, 255, 255, 255);
    rlTexCoord2f(topMin.x, topMin.y); rlVertex2f(pTop.x,      pTop.y);
    rlTexCoord2f(topMin.x, topMax.y); rlVertex2f(pTopLeft.x,  pTopLeft.y);
    rlTexCoord2f(topMax.x, topMax.y); rlVertex2f(pCenter.x,   pCenter.y);
    rlTexCoord2f(topMax.x, topMin.y); rlVertex2f(pTopRight.x, pTopRight.y);

    rlColor4ub(150, 150, 150, 255);
    rlTexCoord2f(sideMin.x, sideMin.y); rlVertex2f(pTopLeft.x,    pTopLeft.y);
    rlTexCoord2f(sideMin.x, sideMax.y); rlVertex2f(pBottomLeft.x, pBottomLeft.y);
    rlTexCoord2f(sideMax.x, sideMax.y); rlVertex2f(pBottom.x,     pBottom.y);
    rlTexCoord2f(sideMax.x, sideMin.y); rlVertex2f(pCenter.x,     pCenter.y);

    rlColor4ub(200, 200, 200, 255);
    rlTexCoord2f(sideMin.x, sideMin.y); rlVertex2f(pCenter.x,      pCenter.y);
    rlTexCoord2f(sideMin.x, sideMax.y); rlVertex2f(pBottom.x,      pBottom.y);
    rlTexCoord2f(sideMax.x, sideMax.y); rlVertex2f(pBottomRight.x, pBottomRight.y);
    rlTexCoord2f(sideMax.x, sideMin.y); rlVertex2f(pTopRight.x,    pTopRight.y);

    rlEnd();
    rlSetTexture(0);
}
