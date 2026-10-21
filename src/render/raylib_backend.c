#include "render/backend.h"
#include "render/rl_compat.h"
#include "core/fileio.h"
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

#define ATLAS_COLS 16.0F
#define ATLAS_ROWS 16.0F

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
  ATLAS_GRID_DIM = 16,
  MIN_TILE_SIZE = 16,
  MAX_TILE_SIZE = 256,
  CHECKER_HALF_SIZE = 8,
  MAX_PACK_FILES = 64,
  RGBA_BYTES_PER_PIXEL = 4
};

static int ComparePackPaths(const void *Lhs, const void *Rhs) {
  const char *PathA = (const char *)Lhs;
  const char *PathB = (const char *)Rhs;
  return strcmp(PathA, PathB);
}

static void SelectTextureDirectories(const char **OutExternalDir,
                                     const char **OutBaseDir) {
  if (DirectoryExists("assets/textures/block")) {
    *OutExternalDir = "assets/textures/external";
    *OutBaseDir = "assets/textures/block";
  } else {
    *OutExternalDir = "textures/external";
    *OutBaseDir = "textures/block";
  }
}

static int OpenExternalPacks(const char *ExternalDir,
                             ZipArchive *OutArchives[MAX_PACK_FILES]) {
  static char CandidatePaths[MAX_PACK_FILES][FILEIO_MAX_PATH];
  static char ZipPaths[MAX_PACK_FILES][FILEIO_MAX_PATH];

  int CandidateCount =
      ListDirectoryFiles(ExternalDir, CandidatePaths, MAX_PACK_FILES);
  int ZipCount = 0;
  for (int Idx = 0; Idx < CandidateCount; Idx++) {
    if (HasFileExtension(CandidatePaths[Idx], ".zip")) {
      snprintf(ZipPaths[ZipCount], FILEIO_MAX_PATH, "%s", CandidatePaths[Idx]);
      ZipCount++;
    }
  }

  if (ZipCount > 1) {
    qsort(ZipPaths, (size_t)ZipCount, FILEIO_MAX_PATH, ComparePackPaths);
  }

  int OpenCount = 0;
  for (int Idx = 0; Idx < ZipCount; Idx++) {
    ZipArchive *Archive = ZipOpen(ZipPaths[Idx]);
    if (Archive == NULL) {
      TraceLog(LOG_WARNING,
               "RENDER: Unusable resource pack archive '%s', skipping",
               ZipPaths[Idx]);
      continue;
    }
    OutArchives[OpenCount++] = Archive;
  }

  return OpenCount;
}

static void CloseExternalPacks(ZipArchive *Archives[MAX_PACK_FILES],
                               int PackCount) {
  for (int Idx = 0; Idx < PackCount; Idx++) {
    ZipClose(Archives[Idx]);
    Archives[Idx] = NULL;
  }
}

static Image TryLoadFromZip(ZipArchive *Archive, const char *EntryPath) {
  Image Result = {0};
  const ZipEntry *Entry = ZipFind(Archive, EntryPath);
  if (Entry == NULL) {
    return Result;
  }

  int RawSize = 0;
  int Method = 0;
  unsigned char *Raw = ZipReadRaw(Archive, Entry, &RawSize, &Method);
  if (Raw == NULL) {
    return Result;
  }

  if (Method == ZIP_METHOD_STORED) {
    Result = LoadImageFromMemory(".png", Raw, RawSize);
  } else if (Method == ZIP_METHOD_DEFLATE) {
    int UncompSize = 0;
    unsigned char *Uncomp = DecompressData(Raw, RawSize, &UncompSize);
    if (Uncomp != NULL && UncompSize > 0) {
      Result = LoadImageFromMemory(".png", Uncomp, UncompSize);
    }
    MemFree(Uncomp);
  }

  free(Raw);
  return Result;
}

static Image ResolveBlockTextureImage(const char *Name, const char *BaseDir,
                                      ZipArchive *Archives[MAX_PACK_FILES],
                                      int PackCount) {
  if (Name != NULL && Name[0] != '\0') {
    char EntryPath[FILEIO_MAX_PATH];
    snprintf(EntryPath, sizeof(EntryPath),
             "assets/minecraft/textures/block/%s.png", Name);

    for (int PackIdx = 0; PackIdx < PackCount; PackIdx++) {
      Image Packed = TryLoadFromZip(Archives[PackIdx], EntryPath);
      if (Packed.data != NULL && Packed.width > 0 && Packed.height > 0) {
        return Packed;
      }
      if (Packed.data != NULL) {
        UnloadImage(Packed);
      }
    }

    char DiskPath[FILEIO_MAX_PATH];
    snprintf(DiskPath, sizeof(DiskPath), "%s/%s.png", BaseDir, Name);
    if (FileExists(DiskPath)) {
      Image BaseImg = LoadImage(DiskPath);
      if (BaseImg.data != NULL && BaseImg.width > 0 && BaseImg.height > 0) {
        return BaseImg;
      }
      if (BaseImg.data != NULL) {
        UnloadImage(BaseImg);
      }
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

  const char *ExternalDir = NULL;
  const char *BaseDir = NULL;
  SelectTextureDirectories(&ExternalDir, &BaseDir);

  ZipArchive *Archives[MAX_PACK_FILES] = {0};
  int PackCount = OpenExternalPacks(ExternalDir, Archives);

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
    Tiles[Idx] = ResolveBlockTextureImage(Name, BaseDir, Archives, PackCount);
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

  CloseExternalPacks(Archives, PackCount);

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

static void SetIconFaceColor(Color8 Tint, unsigned int Shade) {
  unsigned char R =
      (unsigned char)(((unsigned int)Tint.R * Shade) / BLOCK_ICON_COLOR_TOP);
  unsigned char G =
      (unsigned char)(((unsigned int)Tint.G * Shade) / BLOCK_ICON_COLOR_TOP);
  unsigned char B =
      (unsigned char)(((unsigned int)Tint.B * Shade) / BLOCK_ICON_COLOR_TOP);
  rlColor4ub(R, G, B, BLOCK_ICON_ALPHA);
}

static void EmitIconQuad(Vector2 UvMin, Vector2 UvMax, Vector2 V0, Vector2 V1,
                         Vector2 V2, Vector2 V3) {
  rlTexCoord2f(UvMin.x, UvMin.y);
  rlVertex2f(V0.x, V0.y);
  rlTexCoord2f(UvMin.x, UvMax.y);
  rlVertex2f(V1.x, V1.y);
  rlTexCoord2f(UvMax.x, UvMax.y);
  rlVertex2f(V2.x, V2.y);
  rlTexCoord2f(UvMax.x, UvMin.y);
  rlVertex2f(V3.x, V3.y);
}

void RenderDrawBlockIcon(int BlockId, int X, int Y, int Size) {
  BackendState *State = GetBackendState();
  if (BlockId == 0) {
    return;
  }
  BlockType *Def = GetBlockDef(BlockId);
  Color8 Tint = GetBiomeTint(BIOME_FALLBACK_ID, Def->Tint);

  // Cross blocks (flora) have no cube geometry, so the isometric box icon is
  // wrong for them: draw their sprite as a flat, front-facing quad instead.
  if (Def->RenderType == BLOCK_RENDER_CROSS) {
    Vector2 SpriteMin;
    Vector2 SpriteMax;
    GetTextureUV(Def->TexTop, &SpriteMin, &SpriteMax);

    Vector2 TopLeft = {(float)X, (float)Y};
    Vector2 BottomLeft = {(float)X, (float)(Y + Size)};
    Vector2 BottomRight = {(float)(X + Size), (float)(Y + Size)};
    Vector2 TopRight = {(float)(X + Size), (float)Y};

    rlSetTexture(State->BlockAtlas.id);
    rlBegin(RL_QUADS);
    SetIconFaceColor(Tint, BLOCK_ICON_COLOR_TOP);
    EmitIconQuad(SpriteMin, SpriteMax, TopLeft, BottomLeft, BottomRight,
                 TopRight);
    rlEnd();
    rlSetTexture(0);
    return;
  }

  Vector2 TopMin;
  Vector2 TopMax;
  Vector2 SideMin;
  Vector2 SideMax;
  GetTextureUV(Def->TexTop, &TopMin, &TopMax);
  GetTextureUV(Def->TexSide, &SideMin, &SideMax);

  bool HasSideOverlay = (Def->TexSideOverlay != NO_TEXTURE_OVERLAY);
  Color8 SideBaseTint = HasSideOverlay ? COLOR_WHITE : Tint;

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

  SetIconFaceColor(Tint, BLOCK_ICON_COLOR_TOP);
  EmitIconQuad(TopMin, TopMax, PTop, PTopLeft, PCenter, PTopRight);

  SetIconFaceColor(SideBaseTint, BLOCK_ICON_COLOR_LEFT);
  EmitIconQuad(SideMin, SideMax, PTopLeft, PBottomLeft, PBottom, PCenter);

  SetIconFaceColor(SideBaseTint, BLOCK_ICON_COLOR_RIGHT);
  EmitIconQuad(SideMin, SideMax, PCenter, PBottom, PBottomRight, PTopRight);

  if (HasSideOverlay) {
    Vector2 OverlayMin;
    Vector2 OverlayMax;
    GetTextureUV(Def->TexSideOverlay, &OverlayMin, &OverlayMax);

    SetIconFaceColor(Tint, BLOCK_ICON_COLOR_LEFT);
    EmitIconQuad(OverlayMin, OverlayMax, PTopLeft, PBottomLeft, PBottom,
                 PCenter);

    SetIconFaceColor(Tint, BLOCK_ICON_COLOR_RIGHT);
    EmitIconQuad(OverlayMin, OverlayMax, PCenter, PBottom, PBottomRight,
                 PTopRight);
  }

  rlEnd();
  rlSetTexture(0);
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
