#include "render/backend.h"
#include "render/rl_compat.h"
#include "core/fileio.h"
#include "core/image.h"
#include "core/resource_pack.h"
#include "core/profiler.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "world/world.h"
#include "raylib.h"
#include "raymath.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Must include glad before rlgl to get raw GL constants (GL_TEXTURE_2D_ARRAY etc.)
#include "external/glad.h"
#include <rlgl.h>

#define FLOATS_PER_VERTEX 3
#define COLOR_CHANNELS 4

#define BLOCK_HIGHLIGHT_SCALE (BLOCK_SIZE + 0.01F)

// Three meshes per active chunk at most: solid faces, alpha-cutout flora, and
// translucent blocks. Flora was split out of the solid mesh so the two could use
// fragment shaders that differ on the alpha test.
#define MESHES_PER_CHUNK 3
#define MESH_POOL_CAPACITY (MAX_ACTIVE_CHUNKS * MESHES_PER_CHUNK)

typedef struct {
  Texture2D BlockAtlas;
  Texture2D UiAtlas;
  unsigned int BlockArrayTexID;

  // Same vertex shader, two fragment shaders differing only by the alpha test.
  Shader ChunkShaderOpaque;
  Shader ChunkShaderCutout;

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
// Texture array and HUD atlas loader
// ---------------------------------------------------------------------------

enum {
  ATLAS_GRID_DIM = RENDER_BLOCK_ICON_GRID,
  MIN_TILE_SIZE = 16,
  MAX_TILE_SIZE = 256,
  CHECKER_HALF_SIZE = 8,
  RGBA_BYTES_PER_PIXEL = 4
};

// Hands a decoded image to Raylib without copying. stb_image and Raylib both
// allocate through the C runtime's malloc/free, so UnloadImage releases the
// pixels correctly.
static Image ImageFromCore(CoreImage Decoded) {
  Image Result = {0};
  Result.data = Decoded.Pixels;
  Result.width = Decoded.Width;
  Result.height = Decoded.Height;
  Result.mipmaps = 1;
  Result.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
  return Result;
}

static bool TryDecodeFrom(const char *RelPath, int Source, Image *Out) {
  int Size = 0;
  unsigned char *Bytes =
      ResourceReadFrom(RESOURCE_TEXTURE, RelPath, Source, &Size);
  if (Bytes == NULL) {
    return false;
  }
  CoreImage Decoded;
  bool Ok = CoreImageDecodePng(Bytes, Size, &Decoded);
  free(Bytes);
  if (Ok) {
    *Out = ImageFromCore(Decoded);
  }
  return Ok;
}

// A pack whose entry fails to decode is skipped rather than trusted, so the
// next pack or the base directory still gets a chance.
static Image ResolveBlockTextureImage(const char *Name) {
  if (Name != NULL && Name[0] != '\0') {
    char RelPath[FILEIO_MAX_PATH];
    snprintf(RelPath, sizeof(RelPath), "block/%s.png", Name);

    Image Result = {0};
    int PackCount = ResourcePackCount();
    for (int PackIdx = 0; PackIdx < PackCount; PackIdx++) {
      if (TryDecodeFrom(RelPath, PackIdx, &Result)) {
        return Result;
      }
    }
    if (TryDecodeFrom(RelPath, RESOURCE_SOURCE_BASE, &Result)) {
      return Result;
    }
  }

  TraceLog(LOG_WARNING,
           "RENDER: Texture '%s' not found, using checkerboard fallback",
           (Name != NULL) ? Name : "<none>");
  return GenImageChecked(MIN_TILE_SIZE, MIN_TILE_SIZE, CHECKER_HALF_SIZE,
                         CHECKER_HALF_SIZE, MAGENTA, BLACK);
}

static int DeriveTileSize(int MaxObservedSide) {
  int Clamped = MaxObservedSide;
  if (Clamped < MIN_TILE_SIZE) {
    Clamped = MIN_TILE_SIZE;
  }
  if (Clamped > MAX_TILE_SIZE) {
    Clamped = MAX_TILE_SIZE;
  }

  int TileSize = MIN_TILE_SIZE;
  while (TileSize < Clamped && TileSize < MAX_TILE_SIZE) {
    TileSize *= 2;
  }
  return TileSize;
}

static void CopyTileIntoAtlas(Image *AtlasImg, const Image *TileImg,
                              int TileIdx, int TileSize) {
  int TileCol = TileIdx % ATLAS_GRID_DIM;
  int TileRow = TileIdx / ATLAS_GRID_DIM;
  if (TileRow >= ATLAS_GRID_DIM) {
    return;
  }

  unsigned char *DestPixels = (unsigned char *)AtlasImg->data;
  const unsigned char *SrcPixels = (const unsigned char *)TileImg->data;
  size_t RowBytes = (size_t)TileSize * (size_t)RGBA_BYTES_PER_PIXEL;

  for (int Row = 0; Row < TileSize; Row++) {
    int DestY = (TileRow * TileSize) + Row;
    int DestX = TileCol * TileSize;
    size_t DestOffset =
        (((size_t)DestY * (size_t)AtlasImg->width) + (size_t)DestX) *
        (size_t)RGBA_BYTES_PER_PIXEL;
    size_t SrcOffset = (size_t)Row * RowBytes;
    memcpy(DestPixels + DestOffset, SrcPixels + SrcOffset, RowBytes);
  }
}

static void LoadBlockTextureArray(void) {
  BackendState *State = GetBackendState();

  int RegisteredCount = GetBlockTextureCount();
  int LayerCount = (RegisteredCount > 0) ? RegisteredCount : 1;
  if (LayerCount > MAX_BLOCK_TEXTURES) {
    LayerCount = MAX_BLOCK_TEXTURES;
  }

  static Image Tiles[MAX_BLOCK_TEXTURES];
  int MaxSide = MIN_TILE_SIZE;

  for (int Idx = 0; Idx < LayerCount; Idx++) {
    const char *Name =
        (RegisteredCount > 0) ? GetBlockTextureName(Idx) : NULL;
    Tiles[Idx] = ResolveBlockTextureImage(Name);
    ImageFormat(&Tiles[Idx], PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    if (Tiles[Idx].height > Tiles[Idx].width) {
      Rectangle TopFrame = {0.0F, 0.0F, (float)Tiles[Idx].width,
                            (float)Tiles[Idx].width};
      ImageCrop(&Tiles[Idx], TopFrame);
    } else if (Tiles[Idx].width > Tiles[Idx].height) {
      Rectangle LeftFrame = {0.0F, 0.0F, (float)Tiles[Idx].height,
                             (float)Tiles[Idx].height};
      ImageCrop(&Tiles[Idx], LeftFrame);
    }

    if (Tiles[Idx].width > MaxSide) {
      MaxSide = Tiles[Idx].width;
    }
  }

  int TileSize = DeriveTileSize(MaxSide);
  int AtlasDim = ATLAS_GRID_DIM * TileSize;
  Image AtlasImg = GenImageColor(AtlasDim, AtlasDim, BLANK);
  ImageFormat(&AtlasImg, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

  glGenTextures(1, &State->BlockArrayTexID);
  glBindTexture(GL_TEXTURE_2D_ARRAY, State->BlockArrayTexID);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, TileSize, TileSize, LayerCount,
               0, GL_RGBA, GL_UNSIGNED_BYTE, (const void *)0);

  for (int Idx = 0; Idx < LayerCount; Idx++) {
    if (Tiles[Idx].width != TileSize || Tiles[Idx].height != TileSize) {
      ImageResizeNN(&Tiles[Idx], TileSize, TileSize);
    }

    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, Idx, TileSize, TileSize, 1,
                    GL_RGBA, GL_UNSIGNED_BYTE, Tiles[Idx].data);
    CopyTileIntoAtlas(&AtlasImg, &Tiles[Idx], Idx, TileSize);
    UnloadImage(Tiles[Idx]);
  }

  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

  State->BlockAtlas = LoadTextureFromImage(AtlasImg);
  SetTextureFilter(State->BlockAtlas, TEXTURE_FILTER_POINT);
  UnloadImage(AtlasImg);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RenderBackendInit(void) {
  BackendState *State = GetBackendState();

  LoadBlockTextureArray();

  State->ChunkShaderOpaque = LoadShader("assets/shaders/chunk.vert",
                                        "assets/shaders/chunk_opaque.frag");
  State->ChunkShaderCutout = LoadShader("assets/shaders/chunk.vert",
                                        "assets/shaders/chunk_cutout.frag");

  int Unit = 0;
  SetShaderValue(State->ChunkShaderOpaque,
                 GetShaderLocation(State->ChunkShaderOpaque, "uBlockArray"),
                 &Unit, SHADER_UNIFORM_INT);
  SetShaderValue(State->ChunkShaderCutout,
                 GetShaderLocation(State->ChunkShaderCutout, "uBlockArray"),
                 &Unit, SHADER_UNIFORM_INT);

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
  UnloadShader(State->ChunkShaderOpaque);
  UnloadShader(State->ChunkShaderCutout);
  UnloadTexture(State->BlockAtlas);
  if (State->UiAtlas.id != 0) {
    UnloadTexture(State->UiAtlas);
  }
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

  // Covers the staging copies and the driver handoff both, so this scope and
  // the mesh-build scope together account for the whole cost of turning a dirty
  // chunk into a drawable one while staying separable.
  PROFILE_BEGIN(UploadStart);

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

  PROFILE_END(UploadStart, PROFILE_MESH_UPLOAD);
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

// Everything shared by the chunks in a pass, bound once. What is left per chunk
// is the vertex array and the draw -- the only two things that differ.
void RenderBeginChunkPass(ChunkPassKind Kind) {
  BackendState *State = GetBackendState();
  Shader *Active = (Kind == CHUNK_PASS_CUTOUT) ? &State->ChunkShaderCutout
                                               : &State->ChunkShaderOpaque;

  // Raylib batches its own immediate-mode geometry and flushes it lazily. Left
  // pending, it would be drawn later under this pass's shader.
  rlDrawRenderBatchActive();

  rlEnableShader(Active->id);

  Matrix MatMVP = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
  rlSetUniformMatrix(Active->locs[SHADER_LOC_MATRIX_MVP], MatMVP);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, State->BlockArrayTexID);
}

void RenderEndChunkPass(void) {
  rlDisableShader();
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void RenderGetViewProjection(float OutMatrix[RENDER_MATRIX_ELEMENTS]) {
  Matrix M = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());

  // Listed by field name rather than copied over the struct, so the layout is
  // stated here instead of depending on how the backend's matrix type happens to
  // be ordered in memory. Column-major: element[Column * 4 + Row].
  float Source[RENDER_MATRIX_ELEMENTS] = {
      M.m0, M.m1, M.m2,  M.m3,  M.m4,  M.m5,  M.m6,  M.m7,
      M.m8, M.m9, M.m10, M.m11, M.m12, M.m13, M.m14, M.m15};

  memcpy(OutMatrix, Source, sizeof(Source));
}

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

  rlEnableVertexArray(MeshVal->vaoId);
  rlDrawVertexArrayElements(0, MeshVal->triangleCount * 3, 0);
  rlDisableVertexArray();
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void RenderBeginFrame(RenderCamera Camera) {
  PROFILE_BEGIN(SetupStart);
  Camera3D RlCamera = {0};
  RlCamera.position = Vec3ToRL(Camera.Position);
  RlCamera.target = Vec3ToRL(Camera.Target);
  RlCamera.up = Vec3ToRL(Camera.Up);
  RlCamera.fovy = Camera.FovY;
  RlCamera.projection = CAMERA_PERSPECTIVE;

  BeginDrawing();
  ClearBackground(SKYBLUE);
  BeginMode3D(RlCamera);
  PROFILE_END(SetupStart, PROFILE_FRAME_SETUP);
}

void RenderEnd3D(void) { EndMode3D(); }

// EndDrawing is the buffer swap plus the platform's input-event pump, so this
// scope covers everything the driver and the OS can make the frame wait on.
// With no vsync and the GPU queue kept full it is the likeliest home of a stall
// that no other scope accounts for.
void RenderEndFrame(void) {
  PROFILE_BEGIN(PresentStart);
  EndDrawing();
  PROFILE_END(PresentStart, PROFILE_PRESENT);
}

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
// UI layer
// ---------------------------------------------------------------------------

void RenderUploadUiAtlas(const CoreImage *Atlas) {
  BackendState *State = GetBackendState();
  if (State->UiAtlas.id != 0) {
    UnloadTexture(State->UiAtlas);
  }
  Image Wrapped = {0};
  Wrapped.data = Atlas->Pixels;
  Wrapped.width = Atlas->Width;
  Wrapped.height = Atlas->Height;
  Wrapped.mipmaps = 1;
  Wrapped.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
  State->UiAtlas = LoadTextureFromImage(Wrapped);
  SetTextureFilter(State->UiAtlas, TEXTURE_FILTER_POINT);
}

static unsigned int UiTextureHandle(UiTextureId Texture) {
  BackendState *State = GetBackendState();
  return (Texture == UI_TEXTURE_BLOCK_ATLAS) ? State->BlockAtlas.id
                                             : State->UiAtlas.id;
}

void RenderSubmitUI(const UiDrawList *List) {
  for (int CmdIdx = 0; CmdIdx < List->CommandCount; CmdIdx++) {
    const UiCommand *Cmd = &List->Commands[CmdIdx];
    bool Scissored = Cmd->Scissor.W > 0;
    // BeginScissorMode flushes the batch and converts the top-left rectangle
    // to the framebuffer's bottom-left convention.
    if (Scissored) {
      BeginScissorMode(Cmd->Scissor.X, Cmd->Scissor.Y, Cmd->Scissor.W,
                       Cmd->Scissor.H);
    }

    rlSetTexture(UiTextureHandle(Cmd->Texture));
    rlBegin(RL_QUADS);
    for (int QuadIdx = 0; QuadIdx < Cmd->QuadCount; QuadIdx++) {
      // Flushes and restores the mode and texture when the batch is full, so
      // a long chat history cannot overrun it.
      rlCheckRenderBatchLimit(UI_QUAD_CORNERS);
      const UiQuad *Quad = &List->Quads[Cmd->FirstQuad + QuadIdx];
      for (int Corner = 0; Corner < UI_QUAD_CORNERS; Corner++) {
        const UiVertex *Vert = &Quad->V[Corner];
        rlColor4ub(Vert->Color.R, Vert->Color.G, Vert->Color.B, Vert->Color.A);
        rlTexCoord2f(Vert->U, Vert->V);
        rlVertex2f(Vert->X, Vert->Y);
      }
    }
    rlEnd();
    rlSetTexture(0);

    if (Scissored) {
      EndScissorMode();
    }
  }
}

// ---------------------------------------------------------------------------
// 2D drawing layer (rects, text, measure)
// ---------------------------------------------------------------------------

static Color Color8ToRL(Color8 C) { return (Color){C.R, C.G, C.B, C.A}; }

void RenderDrawWireBox(Vec3 Center, Vec3 Size, Color8 Color, bool XRay) {
  Vector3 RlCenter = Vec3ToRL(Center);
  Vector3 RlSize = Vec3ToRL(Size);
  if (XRay) {
    // Flush pending geometry, then draw with depth testing off so the box shows
    // through solid blocks (mirrors the translucent-pass flush pattern).
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    DrawCubeWiresV(RlCenter, RlSize, Color8ToRL(Color));
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
  } else {
    DrawCubeWiresV(RlCenter, RlSize, Color8ToRL(Color));
  }
}

void RenderDrawRect(int X, int Y, int W, int H, Color8 Color) {
  DrawRectangle(X, Y, W, H, Color8ToRL(Color));
}

void RenderDrawRectLines(int X, int Y, int W, int H, Color8 Color) {
  DrawRectangleLines(X, Y, W, H, Color8ToRL(Color));
}

void RenderDrawRectLinesEx(int X, int Y, int W, int H, float Thick, Color8 Color) {
  DrawRectangleLinesEx(
      (Rectangle){(float)X, (float)Y, (float)W, (float)H}, Thick,
      Color8ToRL(Color));
}

void RenderDrawText(const char *Text, int X, int Y, int Size, Color8 Color) {
  DrawText(Text, X, Y, Size, Color8ToRL(Color));
}

int RenderMeasureText(const char *Text, int Size) {
  return MeasureText(Text, Size);
}
