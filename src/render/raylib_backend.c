#include "render/backend.h"
#include "render/rl_compat.h"
#include "world/block_system.h"
#include "world/world.h"
#include "raylib.h"
#include "raymath.h"
#include <string.h>

// Must include glad before rlgl to get raw GL constants (GL_TEXTURE_2D_ARRAY etc.)
#include "external/glad.h"
#include <rlgl.h>

#define ATLAS_COLS 16.0F
#define ATLAS_ROWS 16.0F

#define FLOATS_PER_VERTEX 3
#define COLOR_CHANNELS 4

#define BLOCK_HIGHLIGHT_SCALE (BLOCK_SIZE + 0.01F)

// Two meshes (opaque + translucent) per active chunk at most.
#define MESH_POOL_CAPACITY (MAX_ACTIVE_CHUNKS * 2)

typedef struct {
  Texture2D BlockAtlas;
  unsigned int BlockArrayTexID;
  Shader ChunkShader;

  Mesh MeshPool[MESH_POOL_CAPACITY];
  bool MeshSlotUsed[MESH_POOL_CAPACITY];
  MeshHandle FreeList[MESH_POOL_CAPACITY];
  int FreeCount;
} BackendState;

static BackendState *GetBackendState(void) {
  static BackendState State;
  return &State;
}

// ---------------------------------------------------------------------------
// Texture array loader
// ---------------------------------------------------------------------------

static void LoadBlockTextureArray(void) {
  BackendState *State = GetBackendState();
  Image AtlasImg = LoadImage("assets/atlas/terrain.png");
  ImageFormat(&AtlasImg, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

  int TileW = AtlasImg.width / (int)ATLAS_COLS;
  int TileH = AtlasImg.height / (int)ATLAS_ROWS;
  int TotalTiles = (int)ATLAS_COLS * (int)ATLAS_ROWS;

  glGenTextures(1, &State->BlockArrayTexID);
  glBindTexture(GL_TEXTURE_2D_ARRAY, State->BlockArrayTexID);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, TileW, TileH, TotalTiles, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, (const void *)0);

  unsigned char *Pixels = (unsigned char *)AtlasImg.data;
  unsigned char *TileBuf = (unsigned char *)MemAlloc((size_t)(TileW * TileH * 4));

  for (int TileIdx = 0; TileIdx < TotalTiles; TileIdx++) {
    int TileCol = TileIdx % (int)ATLAS_COLS;
    int TileRow = TileIdx / (int)ATLAS_COLS;

    for (int Row = 0; Row < TileH; Row++) {
      int SrcRow = (TileRow * TileH) + Row;
      int SrcCol = TileCol * TileW;
      size_t DestOffset = (size_t)Row * (size_t)TileW * 4U;
      size_t SrcOffset = (((size_t)SrcRow * (size_t)AtlasImg.width) + (size_t)SrcCol) * 4U;
      size_t Len = (size_t)TileW * 4U;
      memcpy(TileBuf + DestOffset, Pixels + SrcOffset, Len);
    }

    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, TileIdx, TileW, TileH, 1,
                    GL_RGBA, GL_UNSIGNED_BYTE, TileBuf);
  }

  MemFree(TileBuf);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

  UnloadImage(AtlasImg);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RenderBackendInit(void) {
  BackendState *State = GetBackendState();

  State->BlockAtlas = LoadTexture("assets/atlas/terrain.png");
  SetTextureFilter(State->BlockAtlas, TEXTURE_FILTER_POINT);

  LoadBlockTextureArray();

  State->ChunkShader = LoadShader("assets/shaders/chunk.vert", "assets/shaders/chunk.frag");
  int BlockArrayLoc = GetShaderLocation(State->ChunkShader, "uBlockArray");
  int Unit = 0;
  SetShaderValue(State->ChunkShader, BlockArrayLoc, &Unit, SHADER_UNIFORM_INT);

  State->FreeCount = MESH_POOL_CAPACITY;
  for (int IdxI = 0; IdxI < MESH_POOL_CAPACITY; IdxI++) {
    State->MeshSlotUsed[IdxI] = false;
    // Fill the free list in reverse so handles are handed out low-first.
    State->FreeList[IdxI] = (MeshHandle)(MESH_POOL_CAPACITY - 1 - IdxI);
  }
}

void RenderBackendShutdown(void) {
  BackendState *State = GetBackendState();
  for (int IdxI = 0; IdxI < MESH_POOL_CAPACITY; IdxI++) {
    if (State->MeshSlotUsed[IdxI]) {
      UnloadMesh(State->MeshPool[IdxI]);
      State->MeshSlotUsed[IdxI] = false;
    }
  }
  UnloadShader(State->ChunkShader);
  UnloadTexture(State->BlockAtlas);
  glDeleteTextures(1, &State->BlockArrayTexID);
}

// ---------------------------------------------------------------------------
// Mesh pool
// ---------------------------------------------------------------------------

MeshHandle RenderUploadMesh(const MeshData *Data) {
  BackendState *State = GetBackendState();
  if (State->FreeCount == 0) {
    TraceLog(LOG_WARNING, "RENDER: mesh pool exhausted");
    return MESH_HANDLE_INVALID;
  }

  MeshHandle Handle = State->FreeList[--State->FreeCount];
  Mesh MeshVal = {0};
  MeshVal.vertexCount = Data->VertexCount;
  MeshVal.triangleCount = Data->IndexCount / 3;

  size_t VCount = (size_t)Data->VertexCount;
  MeshVal.vertices = (float *)MemAlloc(VCount * FLOATS_PER_VERTEX * sizeof(float));
  memcpy(MeshVal.vertices, Data->Vertices, VCount * FLOATS_PER_VERTEX * sizeof(float));

  MeshVal.indices = (unsigned short *)MemAlloc((size_t)Data->IndexCount * sizeof(unsigned short));
  memcpy(MeshVal.indices, Data->Indices, (size_t)Data->IndexCount * sizeof(unsigned short));

  MeshVal.texcoords = (float *)MemAlloc(VCount * 2 * sizeof(float));
  memcpy(MeshVal.texcoords, Data->TexCoords, VCount * 2 * sizeof(float));

  MeshVal.colors = (unsigned char *)MemAlloc(VCount * COLOR_CHANNELS * sizeof(unsigned char));
  memcpy(MeshVal.colors, Data->Colors, VCount * COLOR_CHANNELS * sizeof(unsigned char));

  MeshVal.texcoords2 = (float *)MemAlloc(VCount * 2 * sizeof(float));
  memcpy(MeshVal.texcoords2, Data->TexLayers, VCount * 2 * sizeof(float));

  UploadMesh(&MeshVal, false);

  State->MeshPool[Handle] = MeshVal;
  State->MeshSlotUsed[Handle] = true;
  return Handle;
}

void RenderFreeMesh(MeshHandle Handle) {
  BackendState *State = GetBackendState();
  if (Handle == MESH_HANDLE_INVALID || Handle >= (MeshHandle)MESH_POOL_CAPACITY) {
    return;
  }
  if (!State->MeshSlotUsed[Handle]) {
    return;
  }
  UnloadMesh(State->MeshPool[Handle]);
  State->MeshSlotUsed[Handle] = false;
  State->FreeList[State->FreeCount++] = Handle;
}

// ---------------------------------------------------------------------------
// Custom draw — bypasses Raylib material/texture binding
// ---------------------------------------------------------------------------

void RenderDrawMesh(MeshHandle Handle) {
  BackendState *State = GetBackendState();
  if (Handle == MESH_HANDLE_INVALID || Handle >= (MeshHandle)MESH_POOL_CAPACITY) {
    return;
  }
  if (!State->MeshSlotUsed[Handle]) {
    return;
  }
  Mesh *MeshVal = &State->MeshPool[Handle];
  if (MeshVal->vaoId == 0) {
    return;
  }

  rlEnableShader(State->ChunkShader.id);

  Matrix MatView = rlGetMatrixModelview();
  Matrix MatProj = rlGetMatrixProjection();
  Matrix MatMVP = MatrixMultiply(MatView, MatProj);
  rlSetUniformMatrix(State->ChunkShader.locs[SHADER_LOC_MATRIX_MVP], MatMVP);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, State->BlockArrayTexID);

  rlEnableVertexArray(MeshVal->vaoId);
  rlDrawVertexArrayElements(0, MeshVal->triangleCount * 3, 0);
  rlDisableVertexArray();

  rlDisableShader();
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void RenderBeginFrame(RenderCamera Camera) {
  Camera3D RlCamera = {0};
  RlCamera.position = Vec3ToRL(Camera.Position);
  RlCamera.target = Vec3ToRL(Camera.Target);
  RlCamera.up = Vec3ToRL(Camera.Up);
  RlCamera.fovy = Camera.FovY;
  RlCamera.projection = CAMERA_PERSPECTIVE;

  BeginDrawing();
  ClearBackground(SKYBLUE);
  BeginMode3D(RlCamera);
}

void RenderEnd3D(void) { EndMode3D(); }

void RenderEndFrame(void) { EndDrawing(); }

void RenderBeginTranslucentPass(void) {
  rlDrawRenderBatchActive();
  rlDisableDepthMask();
  rlEnableColorBlend();
}

void RenderEndTranslucentPass(void) {
  rlDrawRenderBatchActive();
  rlEnableDepthMask();
}

void RenderSetWireframe(bool Enabled) {
  if (Enabled) {
    rlEnableWireMode();
  } else {
    rlDisableWireMode();
  }
}

// ---------------------------------------------------------------------------
// 3D immediate-mode helpers
// ---------------------------------------------------------------------------

void RenderDrawBlockHighlight(Vec3 Pos) {
  DrawCubeWires(Vec3ToRL(Pos), BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE,
                BLOCK_HIGHLIGHT_SCALE, BLACK);
}

void RenderDrawChunkBorder(Vec3 Center, float Size) {
  DrawCubeWires(Vec3ToRL(Center), Size, Size, Size, YELLOW);
}

void RenderDrawDebugCube(Vec3 Pos, float Size, bool Wire, bool Solid) {
  Vector3 RlPos = Vec3ToRL(Pos);
  if (Wire) {
    DrawCubeWires(RlPos, Size, Size, Size, Solid ? PURPLE : ORANGE);
  } else {
    DrawCube(RlPos, Size, Size, Size, Solid ? BLUE : RED);
  }
}

// ---------------------------------------------------------------------------
// 2D block icon (deferred HUD layer)
// ---------------------------------------------------------------------------

#define BLOCK_ICON_HALF_DIVISOR 2.0F
#define BLOCK_ICON_QUART_DIVISOR 4.0F
#define BLOCK_ICON_COLOR_TOP 255U
#define BLOCK_ICON_COLOR_LEFT 150U
#define BLOCK_ICON_COLOR_RIGHT 200U
#define BLOCK_ICON_ALPHA 255U

static void GetTextureUV(int TextureIndex, Vector2 *UvMin, Vector2 *UvMax) {
  float U = (float)(TextureIndex % (int)ATLAS_COLS) / ATLAS_COLS;
  int Row = TextureIndex / (int)ATLAS_COLS;
  float V = (float)Row / ATLAS_ROWS;
  float W = 1.0F / ATLAS_COLS;
  float H = 1.0F / ATLAS_ROWS;
  UvMin->x = U;
  UvMin->y = V;
  UvMax->x = U + W;
  UvMax->y = V + H;
}

void RenderDrawBlockIcon(int BlockId, int X, int Y, int Size) {
  BackendState *State = GetBackendState();
  if (BlockId == 0) {
    return;
  }
  BlockType *Def = GetBlockDef(BlockId);

  Vector2 TopMin;
  Vector2 TopMax;
  Vector2 SideMin;
  Vector2 SideMax;
  GetTextureUV(Def->TexTop, &TopMin, &TopMax);
  GetTextureUV(Def->TexSide, &SideMin, &SideMax);

  float Cx = (float)X + ((float)Size / BLOCK_ICON_HALF_DIVISOR);
  float Cy = (float)Y + ((float)Size / BLOCK_ICON_HALF_DIVISOR);
  float H = (float)Size / BLOCK_ICON_QUART_DIVISOR;

  Vector2 PTop = {Cx, (float)Y};
  Vector2 PTopLeft = {(float)X, (float)Y + H};
  Vector2 PTopRight = {(float)(X + Size), (float)Y + H};
  Vector2 PCenter = {Cx, Cy};
  Vector2 PBottomLeft = {(float)X, Cy + H};
  Vector2 PBottomRight = {(float)(X + Size), Cy + H};
  Vector2 PBottom = {Cx, (float)(Y + Size)};

  rlSetTexture(State->BlockAtlas.id);
  rlBegin(RL_QUADS);

  rlColor4ub(BLOCK_ICON_COLOR_TOP, BLOCK_ICON_COLOR_TOP, BLOCK_ICON_COLOR_TOP, BLOCK_ICON_ALPHA);
  rlTexCoord2f(TopMin.x, TopMin.y); rlVertex2f(PTop.x, PTop.y);
  rlTexCoord2f(TopMin.x, TopMax.y); rlVertex2f(PTopLeft.x, PTopLeft.y);
  rlTexCoord2f(TopMax.x, TopMax.y); rlVertex2f(PCenter.x, PCenter.y);
  rlTexCoord2f(TopMax.x, TopMin.y); rlVertex2f(PTopRight.x, PTopRight.y);

  rlColor4ub(BLOCK_ICON_COLOR_LEFT, BLOCK_ICON_COLOR_LEFT, BLOCK_ICON_COLOR_LEFT, BLOCK_ICON_ALPHA);
  rlTexCoord2f(SideMin.x, SideMin.y); rlVertex2f(PTopLeft.x, PTopLeft.y);
  rlTexCoord2f(SideMin.x, SideMax.y); rlVertex2f(PBottomLeft.x, PBottomLeft.y);
  rlTexCoord2f(SideMax.x, SideMax.y); rlVertex2f(PBottom.x, PBottom.y);
  rlTexCoord2f(SideMax.x, SideMin.y); rlVertex2f(PCenter.x, PCenter.y);

  rlColor4ub(BLOCK_ICON_COLOR_RIGHT, BLOCK_ICON_COLOR_RIGHT, BLOCK_ICON_COLOR_RIGHT, BLOCK_ICON_ALPHA);
  rlTexCoord2f(SideMin.x, SideMin.y); rlVertex2f(PCenter.x, PCenter.y);
  rlTexCoord2f(SideMin.x, SideMax.y); rlVertex2f(PBottom.x, PBottom.y);
  rlTexCoord2f(SideMax.x, SideMax.y); rlVertex2f(PBottomRight.x, PBottomRight.y);
  rlTexCoord2f(SideMax.x, SideMin.y); rlVertex2f(PTopRight.x, PTopRight.y);

  rlEnd();
  rlSetTexture(0);
}
