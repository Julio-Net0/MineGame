#include "render/backend.h"
#include "render/gl_api.h"
#include "render/gl_math.h"
#include "core/fileio.h"
#include "core/image.h"
#include "core/log.h"
#include "core/profiler.h"
#include "core/resource_pack.h"
#include "platform/platform.h"
#include "world/block_system.h"
#include "world/chunk.h"
#include "world/world.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// OpenGL 3.3 core render backend. It owns every piece of GL state it draws
// with and sets that state explicitly each frame, so it depends on nothing a
// windowing library or an earlier draw happened to leave bound.
//
// The conventions it reproduces -- projection, clear colour, blend and depth
// state, debug-line geometry, capture orientation -- are those of the raylib
// backend it replaced, because scripted screenshots taken with that backend are
// the reference every frame is compared against.

#define BLOCK_HIGHLIGHT_SCALE (BLOCK_SIZE + 0.01F)

// Three meshes per active chunk at most: solid faces, alpha-cutout flora, and
// translucent blocks.
#define MESH_POOL_CAPACITY (MAX_ACTIVE_CHUNKS * 3)

enum {
  CHUNK_VERTEX_ATTRIB_LOC = 0,
  CHUNK_VERTEX_WORDS = 2,
  UI_ATTRIB_POSITION = 0,
  UI_ATTRIB_TEXCOORD = 1,
  UI_ATTRIB_COLOR = 2,
  DEBUG_ATTRIB_POSITION = 0,
  DEBUG_ATTRIB_COLOR = 1,
  INFO_LOG_CAPACITY = 2048,
  CAPTURE_ALPHA_OFFSET = 3,
  CAPTURE_OPAQUE_ALPHA = 255
};

// Colours the raylib debug helpers used.
static const Color8 COLOR_HIGHLIGHT = {0, 0, 0, 255};
static const Color8 COLOR_CHUNK_BORDER = {253, 249, 0, 255};
static const Color8 COLOR_DEBUG_WIRE_HIT = {200, 122, 255, 255};
static const Color8 COLOR_DEBUG_WIRE_MISS = {255, 161, 0, 255};
static const Color8 COLOR_DEBUG_SOLID_HIT = {0, 121, 241, 255};
static const Color8 COLOR_DEBUG_SOLID_MISS = {230, 41, 55, 255};
static const Color8 COLOR_CLEAR = {102, 191, 255, 255};

// GPU-only chunk mesh record. Vertex data lives only in Vbo; every chunk Vao
// binds the single shared static EBO populated at startup.
typedef struct {
  GLuint Vao;
  GLuint Vbo;
  int IndexCount;
} ChunkGpuMesh;

typedef struct {
  GLuint Program;
  GLint MvpLoc;
  GLint ChunkOriginLoc;
} ChunkProgram;

// Immediate-mode debug geometry, batched the way raylib's line batch was: in
// submission order, one draw per run of the same primitive, flushed at the
// points that batch flushed so it lands under the same depth and blend state.
enum {
  DEBUG_BATCH_VERTICES = 32768,
  DEBUG_BATCH_SEGMENTS = 256,
  DEBUG_CUBE_WIRE_VERTICES = 24,
  DEBUG_CUBE_SOLID_VERTICES = 36
};

typedef struct {
  float X;
  float Y;
  float Z;
  Color8 Color;
} DebugVertex;

typedef struct {
  GLenum Mode;
  int First;
  int Count;
} DebugSegment;

#ifdef MINEGAME_PROFILE
enum {
  SOLID_QUERY_COUNT = 2
};
#endif

typedef struct {
  GLuint BlockArrayTex;
  GLuint BlockAtlasTex;
  GLuint UiAtlasTex;
  int BlockIconGrid;
  int MissingBlockTextures;

  ChunkProgram ChunkOpaque;
  ChunkProgram ChunkCutout;
  const ChunkProgram *ActiveChunk;
  GLuint BoundChunkVao;

  GLuint UiProgram;
  GLint UiMvpLoc;
  GLuint UiVao;
  GLuint UiVbo;
  GLuint UiEbo;
  int UiQuadCapacity;

  GLuint DebugProgram;
  GLint DebugMvpLoc;
  GLuint DebugVao;
  GLuint DebugVbo;
  DebugVertex DebugVertices[DEBUG_BATCH_VERTICES];
  int DebugVertexCount;
  DebugSegment DebugSegments[DEBUG_BATCH_SEGMENTS];
  int DebugSegmentCount;

  GLuint ChunkSharedEbo;
  ChunkGpuMesh MeshPool[MESH_POOL_CAPACITY];
  bool MeshSlotUsed[MESH_POOL_CAPACITY];
  MeshHandle FreeList[MESH_POOL_CAPACITY];
  int FreeCount;

  // Matrices of the frame being built, column-major.
  float Mvp3D[GL_MAT4_ELEMENTS];
  float Mvp2D[GL_MAT4_ELEMENTS];

  bool CaptureRequested;
  CoreImage Capture;

#ifdef MINEGAME_PROFILE
  // Double-buffered occlusion queries around the solid chunk pass. One slot is
  // written by the current frame while the other, issued at least one frame
  // earlier, is polled with GL_QUERY_RESULT_AVAILABLE so reading it never
  // stalls the pipeline it is measuring.
  GLuint SolidQueries[SOLID_QUERY_COUNT];
  bool SolidQueryPending[SOLID_QUERY_COUNT];
  int SolidQueryCursor;
  bool SolidPassNext;
  bool SolidQueryActive;
  bool HasSolidSamples;
  uint64_t LastSolidSamples;
#endif
} BackendState;

static BackendState *GetBackendState(void) {
  static BackendState State;
  return &State;
}

// OpenGL takes a buffer offset through a pointer parameter. The union states
// that conversion once instead of casting an integer to a pointer at each call.
static const void *GlBufferOffset(size_t Offset) {
  union {
    uintptr_t Offset;
    const void *Pointer;
  } Converted = {.Offset = (uintptr_t)Offset};
  return Converted.Pointer;
}

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

typedef struct {
  GLuint Location;
  const char *Name;
} AttribBinding;

// A program the game cannot draw without. Rendering with a missing or broken
// shader would only show a corrupted frame, so startup stops instead.
static void FailShader(const char *What, const char *Path, const char *InfoLog) {
  LogError("RENDER: %s \"%s\"%s%s", What, Path,
           (InfoLog != NULL) ? ": " : "", (InfoLog != NULL) ? InfoLog : "");
  exit(EXIT_FAILURE);
}

static GLuint CompileStage(GLenum Stage, const char *Path) {
  char *Source = ReadTextFile(Path);
  if (Source == NULL) {
    FailShader("cannot read shader", Path, NULL);
    return 0;
  }
  GLuint Shader = glCreateShader(Stage);
  const GLchar *Sources[1] = {Source};
  glShaderSource(Shader, 1, Sources, NULL);
  glCompileShader(Shader);
  free(Source);

  GLint Compiled = GL_FALSE;
  glGetShaderiv(Shader, GL_COMPILE_STATUS, &Compiled);
  if (Compiled != GL_TRUE) {
    char Log[INFO_LOG_CAPACITY];
    glGetShaderInfoLog(Shader, (GLsizei)sizeof(Log), NULL, Log);
    glDeleteShader(Shader);
    FailShader("failed to compile", Path, Log);
    return 0;
  }
  return Shader;
}

static GLuint LinkProgram(const char *VertexPath, const char *FragmentPath,
                          const AttribBinding *Bindings, int BindingCount) {
  GLuint Vertex = CompileStage(GL_VERTEX_SHADER, VertexPath);
  GLuint Fragment = CompileStage(GL_FRAGMENT_SHADER, FragmentPath);
  GLuint Program = glCreateProgram();
  glAttachShader(Program, Vertex);
  glAttachShader(Program, Fragment);
  for (int Idx = 0; Idx < BindingCount; Idx++) {
    glBindAttribLocation(Program, Bindings[Idx].Location, Bindings[Idx].Name);
  }
  glLinkProgram(Program);
  glDetachShader(Program, Vertex);
  glDetachShader(Program, Fragment);
  glDeleteShader(Vertex);
  glDeleteShader(Fragment);

  GLint Linked = GL_FALSE;
  glGetProgramiv(Program, GL_LINK_STATUS, &Linked);
  if (Linked != GL_TRUE) {
    char Log[INFO_LOG_CAPACITY];
    glGetProgramInfoLog(Program, (GLsizei)sizeof(Log), NULL, Log);
    glDeleteProgram(Program);
    FailShader("failed to link", FragmentPath, Log);
    return 0;
  }
  return Program;
}

// Uniform values persist on the program, so the sampler unit and the overlay
// table are set once here rather than per chunk pass.
static ChunkProgram LoadChunkProgram(const char *FragmentPath) {
  ChunkProgram Result = {0};
  Result.Program = LinkProgram("assets/shaders/chunk.vert", FragmentPath, NULL, 0);
  Result.MvpLoc = glGetUniformLocation(Result.Program, "mvp");
  Result.ChunkOriginLoc = glGetUniformLocation(Result.Program, "uChunkOrigin");

  glUseProgram(Result.Program);
  glUniform1i(glGetUniformLocation(Result.Program, "uBlockArray"), 0);
  glUniform1iv(glGetUniformLocation(Result.Program, "uOverlayLayers"),
               OVERLAY_SLOT_COUNT, GetOverlaySlotLayers());
  glUseProgram(0);
  return Result;
}

// ---------------------------------------------------------------------------
// Texture array and block icon atlas
// ---------------------------------------------------------------------------

enum {
  MIN_ICON_GRID_DIM = 16,
  MIN_TILE_SIZE = 16,
  MAX_TILE_SIZE = 256,
  CHECKER_HALF_SIZE = 8
};

static bool TryDecodeFrom(const char *RelPath, int Source, CoreImage *Out) {
  int Size = 0;
  unsigned char *Bytes =
      ResourceReadFrom(RESOURCE_TEXTURE, RelPath, Source, &Size);
  if (Bytes == NULL) {
    return false;
  }
  bool Ok = CoreImageDecodePng(Bytes, Size, Out);
  free(Bytes);
  return Ok;
}

// A pack whose entry fails to decode is skipped rather than trusted, so the
// next pack or the base directory still gets a chance.
static CoreImage ResolveBlockTextureImage(const char *Name) {
  CoreImage Result = {0};
  if (Name != NULL && Name[0] != '\0') {
    char RelPath[FILEIO_MAX_PATH];
    (void)snprintf(RelPath, sizeof(RelPath), "block/%s.png", Name);

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

  LogWarn("RENDER: Texture '%s' not found, using checkerboard fallback",
          (Name != NULL) ? Name : "<none>");
  // Only a registered name counts: the single placeholder layer an empty
  // registry gets is expected, not missing.
  if (Name != NULL) {
    GetBackendState()->MissingBlockTextures++;
  }
  const Color8 MAGENTA = {255, 0, 255, 255};
  const Color8 BLACK = {0, 0, 0, 255};
  if (!CoreImageCreateChecked(&Result, MIN_TILE_SIZE, MIN_TILE_SIZE,
                              CHECKER_HALF_SIZE, CHECKER_HALF_SIZE, MAGENTA,
                              BLACK)) {
    LogError("RENDER: out of memory building the fallback texture");
  }
  return Result;
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

static void CopyTileIntoAtlas(CoreImage *Atlas, const CoreImage *Tile,
                              int TileIdx, int TileSize, int GridDim) {
  int TileCol = TileIdx % GridDim;
  int TileRow = TileIdx / GridDim;
  if (TileRow >= GridDim || Atlas->Pixels == NULL || Tile->Pixels == NULL) {
    return;
  }

  size_t RowBytes = (size_t)TileSize * (size_t)CORE_IMAGE_CHANNELS;
  for (int Row = 0; Row < TileSize; Row++) {
    int DestY = (TileRow * TileSize) + Row;
    int DestX = TileCol * TileSize;
    size_t DestOffset =
        (((size_t)DestY * (size_t)Atlas->Width) + (size_t)DestX) *
        (size_t)CORE_IMAGE_CHANNELS;
    memcpy(Atlas->Pixels + DestOffset, Tile->Pixels + ((size_t)Row * RowBytes),
           RowBytes);
  }
}

// Smallest square grid holding every layer, never below the original 16x16 so
// small texture sets keep the atlas layout they always had.
static int DeriveIconGridDim(int LayerCount) {
  int Grid = MIN_ICON_GRID_DIM;
  while (Grid * Grid < LayerCount) {
    Grid++;
  }
  return Grid;
}

// Icons are drawn at GUI sizes far below the tile size, so when the full-size
// atlas would exceed the GPU's texture limit the icon tile is halved instead of
// dropping layers from the atlas.
static int DeriveIconTileSize(int TileSize, int GridDim, int MaxTextureSize) {
  int IconTile = TileSize;
  while (IconTile > 1 && GridDim * IconTile > MaxTextureSize) {
    IconTile /= 2;
  }
  return IconTile;
}

static int ClampLayerCountToDriver(int LayerCount) {
  GLint MaxLayers = 0;
  glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &MaxLayers);
  if (MaxLayers > 0 && LayerCount > MaxLayers) {
    LogWarn("RENDER: %d block textures registered but the driver supports only "
            "%d array layers; textures past the limit will sample wrong layers",
            LayerCount, (int)MaxLayers);
    return (int)MaxLayers;
  }
  return LayerCount;
}

// A single-level RGBA8 texture sampled with nearest filtering and repeat
// wrapping, which is what raylib's texture load plus a point filter produced.
static GLuint CreateNearestTexture2D(const CoreImage *Image) {
  GLuint Texture = 0;
  glGenTextures(1, &Texture);
  glBindTexture(GL_TEXTURE_2D, Texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Image->Width, Image->Height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, Image->Pixels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  return Texture;
}

// Square tiles only: a taller image is an animation strip (keep the top
// frame), a wider one keeps its left square.
static void CropToSquare(CoreImage *Tile) {
  if (Tile->Height > Tile->Width) {
    (void)CoreImageCrop(Tile, 0, 0, Tile->Width, Tile->Width);
  } else if (Tile->Width > Tile->Height) {
    (void)CoreImageCrop(Tile, 0, 0, Tile->Height, Tile->Height);
  }
}

static void LoadBlockTextureArray(void) {
  BackendState *State = GetBackendState();

  int RegisteredCount = GetBlockTextureCount();
  int LayerCount = (RegisteredCount > 0) ? RegisteredCount : 1;
  if (LayerCount > MAX_BLOCK_TEXTURES) {
    LayerCount = MAX_BLOCK_TEXTURES;
  }
  LayerCount = ClampLayerCountToDriver(LayerCount);

  static CoreImage Tiles[MAX_BLOCK_TEXTURES];
  int MaxSide = MIN_TILE_SIZE;

  for (int Idx = 0; Idx < LayerCount; Idx++) {
    const char *Name =
        (RegisteredCount > 0) ? GetBlockTextureName(Idx) : NULL;
    Tiles[Idx] = ResolveBlockTextureImage(Name);
    CropToSquare(&Tiles[Idx]);
    if (Tiles[Idx].Width > MaxSide) {
      MaxSide = Tiles[Idx].Width;
    }
  }

  int TileSize = DeriveTileSize(MaxSide);
  GLint MaxTextureSize = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &MaxTextureSize);
  int GridDim = DeriveIconGridDim(LayerCount);
  int IconTile = DeriveIconTileSize(TileSize, GridDim, (int)MaxTextureSize);
  State->BlockIconGrid = GridDim;
  int AtlasDim = GridDim * IconTile;
  CoreImage Atlas = {0};
  if (!CoreImageCreateFilled(&Atlas, AtlasDim, AtlasDim, (Color8){0, 0, 0, 0})) {
    LogError("RENDER: out of memory building the block icon atlas");
  }

  glGenTextures(1, &State->BlockArrayTex);
  glBindTexture(GL_TEXTURE_2D_ARRAY, State->BlockArrayTex);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, TileSize, TileSize, LayerCount,
               0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  for (int Idx = 0; Idx < LayerCount; Idx++) {
    CoreImage *Tile = &Tiles[Idx];
    if (Tile->Width != TileSize || Tile->Height != TileSize) {
      (void)CoreImageResizeNearest(Tile, TileSize, TileSize);
    }
    if (Tile->Pixels != NULL) {
      glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, Idx, TileSize, TileSize, 1,
                      GL_RGBA, GL_UNSIGNED_BYTE, Tile->Pixels);
    }
    if (IconTile != TileSize) {
      (void)CoreImageResizeNearest(Tile, IconTile, IconTile);
    }
    CopyTileIntoAtlas(&Atlas, Tile, Idx, IconTile, GridDim);
    CoreImageFree(Tile);
  }

  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

  if (Atlas.Pixels != NULL) {
    State->BlockAtlasTex = CreateNearestTexture2D(&Atlas);
  }
  CoreImageFree(&Atlas);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Every quad in a chunk mesh uses the standard (0, 1, 2, 0, 2, 3) index split
// (AO-flipped quads rotate their four vertices at emit time), so one static
// 16-bit index buffer serves every chunk VAO in the engine.
static void InitSharedChunkIndexBuffer(BackendState *State) {
  unsigned short *Indices = (unsigned short *)malloc(
      (size_t)CHUNK_MAX_INDICES * sizeof(unsigned short));
  if (Indices == NULL) {
    LogError("RENDER: out of memory allocating shared chunk index buffer");
    return;
  }

  static const unsigned short QUAD_PATTERN[CHUNK_INDICES_PER_QUAD] = {
      0, 1, 2, 0, 2, 3};
  for (int Quad = 0; Quad < CHUNK_MAX_QUADS; Quad++) {
    unsigned short Base = (unsigned short)(Quad * CHUNK_VERTICES_PER_QUAD);
    unsigned short *Out = &Indices[(size_t)Quad * CHUNK_INDICES_PER_QUAD];
    for (int Corner = 0; Corner < CHUNK_INDICES_PER_QUAD; Corner++) {
      Out[Corner] = (unsigned short)(Base + QUAD_PATTERN[Corner]);
    }
  }

  glGenBuffers(1, &State->ChunkSharedEbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, State->ChunkSharedEbo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               (GLsizeiptr)((size_t)CHUNK_MAX_INDICES * sizeof(unsigned short)),
               Indices, GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  free(Indices);
}

static void InitDebugBatch(BackendState *State) {
  static const AttribBinding BINDINGS[] = {
      {DEBUG_ATTRIB_POSITION, "vertexPosition"},
      {DEBUG_ATTRIB_COLOR, "vertexColor"}};
  State->DebugProgram = LinkProgram("assets/shaders/debug.vert",
                                    "assets/shaders/debug.frag", BINDINGS,
                                    (int)(sizeof(BINDINGS) / sizeof(BINDINGS[0])));
  State->DebugMvpLoc = glGetUniformLocation(State->DebugProgram, "mvp");

  glGenVertexArrays(1, &State->DebugVao);
  glGenBuffers(1, &State->DebugVbo);
  glBindVertexArray(State->DebugVao);
  glBindBuffer(GL_ARRAY_BUFFER, State->DebugVbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(State->DebugVertices), NULL,
               GL_STREAM_DRAW);
  glEnableVertexAttribArray(DEBUG_ATTRIB_POSITION);
  glVertexAttribPointer(DEBUG_ATTRIB_POSITION, 3, GL_FLOAT, GL_FALSE,
                        (GLsizei)sizeof(DebugVertex),
                        GlBufferOffset(offsetof(DebugVertex, X)));
  glEnableVertexAttribArray(DEBUG_ATTRIB_COLOR);
  glVertexAttribPointer(DEBUG_ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                        (GLsizei)sizeof(DebugVertex),
                        GlBufferOffset(offsetof(DebugVertex, Color)));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RenderBackendInit(void) {
  BackendState *State = GetBackendState();

  LoadBlockTextureArray();

  State->ChunkOpaque = LoadChunkProgram("assets/shaders/chunk_opaque.frag");
  State->ChunkCutout = LoadChunkProgram("assets/shaders/chunk_cutout.frag");
  State->ActiveChunk = NULL;

  static const AttribBinding UI_BINDINGS[] = {
      {UI_ATTRIB_POSITION, "vertexPosition"},
      {UI_ATTRIB_TEXCOORD, "vertexTexCoord"},
      {UI_ATTRIB_COLOR, "vertexColor"}};
  State->UiProgram = LinkProgram(
      "assets/shaders/ui.vert", "assets/shaders/ui.frag", UI_BINDINGS,
      (int)(sizeof(UI_BINDINGS) / sizeof(UI_BINDINGS[0])));
  State->UiMvpLoc = glGetUniformLocation(State->UiProgram, "mvp");
  glUseProgram(State->UiProgram);
  glUniform1i(glGetUniformLocation(State->UiProgram, "texture0"), 0);
  glUseProgram(0);

  InitDebugBatch(State);
  InitSharedChunkIndexBuffer(State);

  State->FreeCount = MESH_POOL_CAPACITY;
  for (int IdxI = 0; IdxI < MESH_POOL_CAPACITY; IdxI++) {
    State->MeshPool[IdxI] = (ChunkGpuMesh){0};
    State->MeshSlotUsed[IdxI] = false;
    // Fill the free list in reverse so handles are handed out low-first.
    State->FreeList[IdxI] = (MeshHandle)(MESH_POOL_CAPACITY - 1 - IdxI);
  }

#ifdef MINEGAME_PROFILE
  glGenQueries(SOLID_QUERY_COUNT, State->SolidQueries);
  for (int Idx = 0; Idx < SOLID_QUERY_COUNT; Idx++) {
    State->SolidQueryPending[Idx] = false;
  }
  State->SolidQueryCursor = 0;
  State->SolidPassNext = false;
  State->SolidQueryActive = false;
  State->HasSolidSamples = false;
  State->LastSolidSamples = 0;
#endif
}

static void DeleteTexture(GLuint *Texture) {
  if (*Texture != 0) {
    glDeleteTextures(1, Texture);
    *Texture = 0;
  }
}

static void DeleteProgram(GLuint *Program) {
  if (*Program != 0) {
    glDeleteProgram(*Program);
    *Program = 0;
  }
}

void RenderBackendShutdown(void) {
  BackendState *State = GetBackendState();
  CoreImageFree(&State->Capture);
  State->CaptureRequested = false;
  for (int IdxI = 0; IdxI < MESH_POOL_CAPACITY; IdxI++) {
    if (State->MeshSlotUsed[IdxI]) {
      glDeleteVertexArrays(1, &State->MeshPool[IdxI].Vao);
      glDeleteBuffers(1, &State->MeshPool[IdxI].Vbo);
      State->MeshPool[IdxI] = (ChunkGpuMesh){0};
      State->MeshSlotUsed[IdxI] = false;
    }
  }
  if (State->ChunkSharedEbo != 0) {
    glDeleteBuffers(1, &State->ChunkSharedEbo);
    State->ChunkSharedEbo = 0;
  }
  DeleteProgram(&State->ChunkOpaque.Program);
  DeleteProgram(&State->ChunkCutout.Program);
  DeleteProgram(&State->UiProgram);
  DeleteProgram(&State->DebugProgram);
  if (State->UiVao != 0) {
    glDeleteVertexArrays(1, &State->UiVao);
    glDeleteBuffers(1, &State->UiVbo);
    glDeleteBuffers(1, &State->UiEbo);
    State->UiVao = 0;
    State->UiQuadCapacity = 0;
  }
  if (State->DebugVao != 0) {
    glDeleteVertexArrays(1, &State->DebugVao);
    glDeleteBuffers(1, &State->DebugVbo);
    State->DebugVao = 0;
  }
#ifdef MINEGAME_PROFILE
  if (State->SolidQueries[0] != 0) {
    glDeleteQueries(SOLID_QUERY_COUNT, State->SolidQueries);
    for (int Idx = 0; Idx < SOLID_QUERY_COUNT; Idx++) {
      State->SolidQueries[Idx] = 0;
      State->SolidQueryPending[Idx] = false;
    }
  }
  State->HasSolidSamples = false;
#endif
  DeleteTexture(&State->BlockAtlasTex);
  DeleteTexture(&State->UiAtlasTex);
  DeleteTexture(&State->BlockArrayTex);
}

// ---------------------------------------------------------------------------
// Mesh pool
// ---------------------------------------------------------------------------

MeshHandle RenderUploadMesh(const MeshData *Data) {
  BackendState *State = GetBackendState();
  if (Data == NULL || Data->Vertices == NULL || Data->VertexCount <= 0) {
    return MESH_HANDLE_INVALID;
  }
  if (State->FreeCount == 0) {
    LogWarn("RENDER: mesh pool exhausted");
    return MESH_HANDLE_INVALID;
  }

  // Covers the driver handoff so this scope and the mesh-build scope together
  // account for the whole cost of turning a dirty chunk into a drawable one
  // while staying separable.
  PROFILE_BEGIN(UploadStart);

  MeshHandle Handle = State->FreeList[--State->FreeCount];
  ChunkGpuMesh MeshVal = {0};
  int QuadCount = Data->VertexCount / CHUNK_VERTICES_PER_QUAD;
  MeshVal.IndexCount = QuadCount * CHUNK_INDICES_PER_QUAD;

  glGenVertexArrays(1, &MeshVal.Vao);
  glGenBuffers(1, &MeshVal.Vbo);

  glBindVertexArray(MeshVal.Vao);
  glBindBuffer(GL_ARRAY_BUFFER, MeshVal.Vbo);
  glBufferData(GL_ARRAY_BUFFER,
               (GLsizeiptr)((size_t)Data->VertexCount * sizeof(ChunkVertex)),
               Data->Vertices, GL_STATIC_DRAW);
  glEnableVertexAttribArray(CHUNK_VERTEX_ATTRIB_LOC);
  glVertexAttribIPointer(CHUNK_VERTEX_ATTRIB_LOC, CHUNK_VERTEX_WORDS,
                         GL_UNSIGNED_INT, (GLsizei)sizeof(ChunkVertex),
                         GlBufferOffset(0));
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, State->ChunkSharedEbo);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

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
  glDeleteVertexArrays(1, &State->MeshPool[Handle].Vao);
  glDeleteBuffers(1, &State->MeshPool[Handle].Vbo);
  State->MeshPool[Handle] = (ChunkGpuMesh){0};
  State->MeshSlotUsed[Handle] = false;
  State->FreeList[State->FreeCount++] = Handle;
}

// ---------------------------------------------------------------------------
// Debug batch
// ---------------------------------------------------------------------------

static void FlushDebugBatch(void) {
  BackendState *State = GetBackendState();
  if (State->DebugVertexCount == 0 || State->DebugProgram == 0) {
    State->DebugVertexCount = 0;
    State->DebugSegmentCount = 0;
    return;
  }

  glUseProgram(State->DebugProgram);
  glUniformMatrix4fv(State->DebugMvpLoc, 1, GL_FALSE, State->Mvp3D);
  glBindVertexArray(State->DebugVao);
  glBindBuffer(GL_ARRAY_BUFFER, State->DebugVbo);
  // Orphaned so the upload never waits on the previous flush still drawing.
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(State->DebugVertices), NULL,
               GL_STREAM_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, 0,
                  (GLsizeiptr)((size_t)State->DebugVertexCount * sizeof(DebugVertex)),
                  State->DebugVertices);
  for (int Seg = 0; Seg < State->DebugSegmentCount; Seg++) {
    const DebugSegment *Segment = &State->DebugSegments[Seg];
    glDrawArrays(Segment->Mode, Segment->First, Segment->Count);
  }
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glUseProgram(0);

  State->DebugVertexCount = 0;
  State->DebugSegmentCount = 0;
}

// Opens room for Count vertices of Mode, continuing the current run when the
// primitive matches. Primitives are never split across a flush. NULL only if
// Count alone exceeds the batch.
static DebugVertex *ReserveDebugVertices(GLenum Mode, int Count) {
  BackendState *State = GetBackendState();
  if (Count > DEBUG_BATCH_VERTICES) {
    return NULL;
  }
  if (State->DebugVertexCount + Count > DEBUG_BATCH_VERTICES) {
    FlushDebugBatch();
  }
  int SegmentCount = State->DebugSegmentCount;
  if (SegmentCount == 0 || State->DebugSegments[SegmentCount - 1].Mode != Mode) {
    if (SegmentCount >= DEBUG_BATCH_SEGMENTS) {
      FlushDebugBatch();
      SegmentCount = 0;
    }
    State->DebugSegments[SegmentCount] =
        (DebugSegment){.Mode = Mode, .First = State->DebugVertexCount, .Count = 0};
    SegmentCount++;
    State->DebugSegmentCount = SegmentCount;
  }
  DebugVertex *Out = &State->DebugVertices[State->DebugVertexCount];
  State->DebugVertexCount += Count;
  State->DebugSegments[SegmentCount - 1].Count += Count;
  return Out;
}

// A local cube corner placed at Pos the way raylib's rlTranslatef transform
// placed it: the full matrix-vector product, including its zero terms.
static DebugVertex PlaceCorner(Vec3 Pos, const float Local[3], Color8 Color) {
  DebugVertex Vertex;
  Vertex.X = (1.0F * Local[0]) + (0.0F * Local[1]) + (0.0F * Local[2]) + Pos.x;
  Vertex.Y = (0.0F * Local[0]) + (1.0F * Local[1]) + (0.0F * Local[2]) + Pos.y;
  Vertex.Z = (0.0F * Local[0]) + (0.0F * Local[1]) + (1.0F * Local[2]) + Pos.z;
  Vertex.Color = Color;
  return Vertex;
}

// The two coordinates a cube of the given size spans on one axis, centred on
// zero, computed as raylib wrote them (x - width/2, x + width/2 with x = 0).
typedef struct {
  float Low;
  float High;
} AxisSpan;

static AxisSpan CentredSpan(float Size) {
  return (AxisSpan){.Low = 0.0F - (Size / 2), .High = 0.0F + (Size / 2)};
}

// raylib's DrawCubeWires, edge for edge and endpoint for endpoint: the order
// of a line's endpoints decides which pixels the rasterizer lights.
static void AppendCubeWires(Vec3 Pos, float Width, float Height, float Length,
                            Color8 Color) {
  AxisSpan X = CentredSpan(Width);
  AxisSpan Y = CentredSpan(Height);
  AxisSpan Z = CentredSpan(Length);
  float Corners[DEBUG_CUBE_WIRE_VERTICES][3] = {
      {X.Low, Y.Low, Z.High},   {X.High, Y.Low, Z.High},
      {X.High, Y.Low, Z.High},  {X.High, Y.High, Z.High},
      {X.High, Y.High, Z.High}, {X.Low, Y.High, Z.High},
      {X.Low, Y.High, Z.High},  {X.Low, Y.Low, Z.High},
      {X.Low, Y.Low, Z.Low},    {X.High, Y.Low, Z.Low},
      {X.High, Y.Low, Z.Low},   {X.High, Y.High, Z.Low},
      {X.High, Y.High, Z.Low},  {X.Low, Y.High, Z.Low},
      {X.Low, Y.High, Z.Low},   {X.Low, Y.Low, Z.Low},
      {X.Low, Y.High, Z.High},  {X.Low, Y.High, Z.Low},
      {X.High, Y.High, Z.High}, {X.High, Y.High, Z.Low},
      {X.Low, Y.Low, Z.High},   {X.Low, Y.Low, Z.Low},
      {X.High, Y.Low, Z.High},  {X.High, Y.Low, Z.Low}};
  DebugVertex *Out = ReserveDebugVertices(GL_LINES, DEBUG_CUBE_WIRE_VERTICES);
  if (Out == NULL) {
    return;
  }
  for (int Idx = 0; Idx < DEBUG_CUBE_WIRE_VERTICES; Idx++) {
    Out[Idx] = PlaceCorner(Pos, Corners[Idx], Color);
  }
}

// raylib's DrawCube triangle list, in its winding.
static void AppendCubeSolid(Vec3 Pos, float Width, float Height, float Length,
                            Color8 Color) {
  AxisSpan X = CentredSpan(Width);
  AxisSpan Y = CentredSpan(Height);
  AxisSpan Z = CentredSpan(Length);
  float Corners[DEBUG_CUBE_SOLID_VERTICES][3] = {
      // Front
      {X.Low, Y.Low, Z.High}, {X.High, Y.Low, Z.High}, {X.Low, Y.High, Z.High},
      {X.High, Y.High, Z.High}, {X.Low, Y.High, Z.High}, {X.High, Y.Low, Z.High},
      // Back
      {X.Low, Y.Low, Z.Low}, {X.Low, Y.High, Z.Low}, {X.High, Y.Low, Z.Low},
      {X.High, Y.High, Z.Low}, {X.High, Y.Low, Z.Low}, {X.Low, Y.High, Z.Low},
      // Top
      {X.Low, Y.High, Z.Low}, {X.Low, Y.High, Z.High}, {X.High, Y.High, Z.High},
      {X.High, Y.High, Z.Low}, {X.Low, Y.High, Z.Low}, {X.High, Y.High, Z.High},
      // Bottom
      {X.Low, Y.Low, Z.Low}, {X.High, Y.Low, Z.High}, {X.Low, Y.Low, Z.High},
      {X.High, Y.Low, Z.Low}, {X.High, Y.Low, Z.High}, {X.Low, Y.Low, Z.Low},
      // Right
      {X.High, Y.Low, Z.Low}, {X.High, Y.High, Z.Low}, {X.High, Y.High, Z.High},
      {X.High, Y.Low, Z.High}, {X.High, Y.Low, Z.Low}, {X.High, Y.High, Z.High},
      // Left
      {X.Low, Y.Low, Z.Low}, {X.Low, Y.High, Z.High}, {X.Low, Y.High, Z.Low},
      {X.Low, Y.Low, Z.High}, {X.Low, Y.High, Z.High}, {X.Low, Y.Low, Z.Low}};
  DebugVertex *Out = ReserveDebugVertices(GL_TRIANGLES, DEBUG_CUBE_SOLID_VERTICES);
  if (Out == NULL) {
    return;
  }
  for (int Idx = 0; Idx < DEBUG_CUBE_SOLID_VERTICES; Idx++) {
    Out[Idx] = PlaceCorner(Pos, Corners[Idx], Color);
  }
}

// ---------------------------------------------------------------------------
// Chunk passes
// ---------------------------------------------------------------------------

#ifdef MINEGAME_PROFILE
static void BeginSolidOcclusionQuery(BackendState *State) {
  if (!State->SolidPassNext) {
    return;
  }
  State->SolidPassNext = false;

  int Slot = State->SolidQueryCursor;
  if (State->SolidQueries[Slot] == 0) {
    return;
  }

  // Harvest the earlier frame's result from this slot before reusing it. If
  // the GPU has not finished that frame yet, skip issuing a new query rather
  // than blocking on GL_QUERY_RESULT and stalling the pipeline we are trying
  // to measure.
  if (State->SolidQueryPending[Slot]) {
    GLint Available = 0;
    glGetQueryObjectiv(State->SolidQueries[Slot], GL_QUERY_RESULT_AVAILABLE,
                       &Available);
    if (Available == 0) {
      return;
    }
    GLuint Passed = 0;
    glGetQueryObjectuiv(State->SolidQueries[Slot], GL_QUERY_RESULT, &Passed);
    State->LastSolidSamples = (uint64_t)Passed;
    State->HasSolidSamples = true;
    State->SolidQueryPending[Slot] = false;
  }

  glBeginQuery(GL_SAMPLES_PASSED, State->SolidQueries[Slot]);
  State->SolidQueryActive = true;
}

static void EndSolidOcclusionQuery(BackendState *State) {
  if (!State->SolidQueryActive) {
    return;
  }
  glEndQuery(GL_SAMPLES_PASSED);
  State->SolidQueryActive = false;

  int Slot = State->SolidQueryCursor;
  State->SolidQueryPending[Slot] = true;
  State->SolidQueryCursor = (Slot + 1) % SOLID_QUERY_COUNT;
}
#endif

// Everything shared by the chunks in a pass, bound once. What is left per chunk
// is the chunk origin uniform, the vertex array, and the draw.
void RenderBeginChunkPass(ChunkPassKind Kind) {
  BackendState *State = GetBackendState();
  // Debug geometry queued so far was drawn before this pass under the old
  // backend; drawing it later would change what it is depth-tested against.
  FlushDebugBatch();

  State->ActiveChunk =
      (Kind == CHUNK_PASS_CUTOUT) ? &State->ChunkCutout : &State->ChunkOpaque;
  State->BoundChunkVao = 0;

  glUseProgram(State->ActiveChunk->Program);
  glUniformMatrix4fv(State->ActiveChunk->MvpLoc, 1, GL_FALSE, State->Mvp3D);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, State->BlockArrayTex);

#ifdef MINEGAME_PROFILE
  BeginSolidOcclusionQuery(State);
#endif
}

void RenderEndChunkPass(void) {
  BackendState *State = GetBackendState();
#ifdef MINEGAME_PROFILE
  EndSolidOcclusionQuery(State);
#endif
  State->ActiveChunk = NULL;
  // Released once per pass rather than after every draw, so no buffer bound
  // outside a pass (a mesh upload, the debug batch) is recorded into a chunk VAO.
  if (State->BoundChunkVao != 0) {
    glBindVertexArray(0);
    State->BoundChunkVao = 0;
  }
  glUseProgram(0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

bool RenderQuerySolidSamples(uint64_t *OutSamples, float *OutOverdraw) {
  if (OutSamples != (uint64_t *)0) {
    *OutSamples = 0;
  }
  if (OutOverdraw != (float *)0) {
    *OutOverdraw = 0.0F;
  }
#ifdef MINEGAME_PROFILE
  const BackendState *State = GetBackendState();
  if (!State->HasSolidSamples) {
    return false;
  }
  int Width = 0;
  int Height = 0;
  PlatformGetFramebufferSize(&Width, &Height);
  if (Width <= 0 || Height <= 0) {
    return false;
  }
  uint64_t Pixels = (uint64_t)Width * (uint64_t)Height;
  if (OutSamples != (uint64_t *)0) {
    *OutSamples = State->LastSolidSamples;
  }
  if (OutOverdraw != (float *)0) {
    *OutOverdraw = (float)((double)State->LastSolidSamples / (double)Pixels);
  }
  return true;
#else
  return false;
#endif
}

void RenderGetViewProjection(float OutMatrix[RENDER_MATRIX_ELEMENTS]) {
  memcpy(OutMatrix, GetBackendState()->Mvp3D,
         sizeof(float) * RENDER_MATRIX_ELEMENTS);
}

void RenderDrawMesh(MeshHandle Handle, Vec3 ChunkOrigin) {
  BackendState *State = GetBackendState();
  if (Handle == MESH_HANDLE_INVALID || Handle >= (MeshHandle)MESH_POOL_CAPACITY) {
    return;
  }
  if (!State->MeshSlotUsed[Handle] || State->ActiveChunk == NULL) {
    return;
  }
  const ChunkGpuMesh *MeshVal = &State->MeshPool[Handle];
  if (MeshVal->Vao == 0 || MeshVal->IndexCount <= 0) {
    return;
  }

  if (State->ActiveChunk->ChunkOriginLoc >= 0) {
    glUniform3f(State->ActiveChunk->ChunkOriginLoc, ChunkOrigin.x,
                ChunkOrigin.y, ChunkOrigin.z);
  }
  if (State->BoundChunkVao != MeshVal->Vao) {
    glBindVertexArray(MeshVal->Vao);
    State->BoundChunkVao = MeshVal->Vao;
  }
  glDrawElements(GL_TRIANGLES, MeshVal->IndexCount, GL_UNSIGNED_SHORT,
                 GlBufferOffset(0));
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void RenderBeginFrame(RenderCamera Camera) {
  PROFILE_BEGIN(SetupStart);
  BackendState *State = GetBackendState();
#ifdef MINEGAME_PROFILE
  State->SolidPassNext = true;
#endif
  int Width = 0;
  int Height = 0;
  PlatformGetFramebufferSize(&Width, &Height);
  if (Width <= 0 || Height <= 0) {
    Width = 1;
    Height = 1;
  }

  GlMat4ToColumnMajor(GlMat4PerspectiveViewProjection(Camera.Position,
                                                      Camera.Target, Camera.Up,
                                                      Camera.FovY, Width, Height),
                      State->Mvp3D);
  GlMat4ToColumnMajor(GlMat4ScreenProjection(PlatformGetScreenWidth(),
                                             PlatformGetScreenHeight()),
                      State->Mvp2D);

  // Every piece of fixed-function state the frame relies on, set here rather
  // than assumed: nothing that ran before this frame is trusted to have left
  // it as the frame needs it.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, Width, Height);
  glDisable(GL_SCISSOR_TEST);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glDepthMask(GL_TRUE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor((float)COLOR_CLEAR.R / COLOR8_CHANNEL_MAX,
               (float)COLOR_CLEAR.G / COLOR8_CHANNEL_MAX,
               (float)COLOR_CLEAR.B / COLOR8_CHANNEL_MAX,
               (float)COLOR_CLEAR.A / COLOR8_CHANNEL_MAX);
  glClearDepth(1.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glDepthFunc(GL_LEQUAL);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glCullFace(GL_BACK);
  glFrontFace(GL_CCW);
  glEnable(GL_CULL_FACE);
  glActiveTexture(GL_TEXTURE0);
  PROFILE_END(SetupStart, PROFILE_FRAME_SETUP);
}

void RenderEnd3D(void) {
  FlushDebugBatch();
  glDisable(GL_DEPTH_TEST);
}

void RenderRequestCapture(void) {
  GetBackendState()->CaptureRequested = true;
}

bool RenderTakeCapture(CoreImage *Out) {
  BackendState *State = GetBackendState();
  if (Out == (CoreImage *)0 || State->Capture.Pixels == (unsigned char *)0) {
    return false;
  }
  *Out = State->Capture;
  State->Capture = (CoreImage){0};
  return true;
}

// Reads the frame back before it is swapped away. Left outside the present
// scope below on purpose: the read is a full GPU sync, and folding it into the
// present timing would make the one frame that captures look like a stall in
// the profiler's numbers for every frame.
static void CaptureFrameIfRequested(void) {
  BackendState *State = GetBackendState();
  if (!State->CaptureRequested) {
    return;
  }
  State->CaptureRequested = false;

  int Width = 0;
  int Height = 0;
  PlatformGetFramebufferSize(&Width, &Height);
  if (Width <= 0 || Height <= 0) {
    return;
  }

  size_t RowBytes = (size_t)Width * CORE_IMAGE_CHANNELS;
  size_t Bytes = RowBytes * (size_t)Height;
  unsigned char *Raw = (unsigned char *)malloc(Bytes);
  unsigned char *Owned = (unsigned char *)malloc(Bytes);
  if (Raw == (unsigned char *)0 || Owned == (unsigned char *)0) {
    LogError("RENDER: out of memory capturing a %dx%d frame", Width, Height);
    free(Raw);
    free(Owned);
    return;
  }

  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadBuffer(GL_BACK);
  glReadPixels(0, 0, Width, Height, GL_RGBA, GL_UNSIGNED_BYTE, Raw);

  // GL returns rows bottom-up; a CoreImage is top row first. Alpha is forced
  // opaque: blending has already applied it to the colour.
  for (int Row = 0; Row < Height; Row++) {
    unsigned char *Dst = Owned + ((size_t)Row * RowBytes);
    memcpy(Dst, Raw + ((size_t)(Height - 1 - Row) * RowBytes), RowBytes);
    for (size_t Px = CAPTURE_ALPHA_OFFSET; Px < RowBytes;
         Px += CORE_IMAGE_CHANNELS) {
      Dst[Px] = CAPTURE_OPAQUE_ALPHA;
    }
  }
  free(Raw);

  // A capture nobody took yet is replaced, not leaked: the newer frame is the
  // one that was asked for.
  CoreImageFree(&State->Capture);
  State->Capture.Width = Width;
  State->Capture.Height = Height;
  State->Capture.Pixels = Owned;
}

// The present is the buffer swap plus the platform's input-event pump, so this
// scope covers everything the driver and the OS can make the frame wait on.
// With no vsync and the GPU queue kept full it is the likeliest home of a stall
// that no other scope accounts for.
void RenderEndFrame(void) {
  FlushDebugBatch();
  CaptureFrameIfRequested();

  PROFILE_BEGIN(PresentStart);
  PlatformPresent();
  PROFILE_END(PresentStart, PROFILE_PRESENT);
}

void RenderBeginTranslucentPass(void) {
  FlushDebugBatch();
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
}

void RenderEndTranslucentPass(void) {
  FlushDebugBatch();
  glDepthMask(GL_TRUE);
}

void RenderSetWireframe(bool Enabled) {
  glPolygonMode(GL_FRONT_AND_BACK, Enabled ? GL_LINE : GL_FILL);
}

// ---------------------------------------------------------------------------
// 3D immediate-mode helpers
// ---------------------------------------------------------------------------

void RenderDrawBlockHighlight(Vec3 Pos) {
  AppendCubeWires(Pos, BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE,
                  BLOCK_HIGHLIGHT_SCALE, COLOR_HIGHLIGHT);
}

void RenderDrawChunkBorder(Vec3 Center, float Size) {
  AppendCubeWires(Center, Size, Size, Size, COLOR_CHUNK_BORDER);
}

void RenderDrawDebugCube(Vec3 Pos, float Size, bool Wire, bool Solid) {
  if (Wire) {
    AppendCubeWires(Pos, Size, Size, Size,
                    Solid ? COLOR_DEBUG_WIRE_HIT : COLOR_DEBUG_WIRE_MISS);
  } else {
    AppendCubeSolid(Pos, Size, Size, Size,
                    Solid ? COLOR_DEBUG_SOLID_HIT : COLOR_DEBUG_SOLID_MISS);
  }
}

void RenderDrawWireBox(Vec3 Center, Vec3 Size, Color8 Color, bool XRay) {
  if (XRay) {
    // Drawn on its own with depth testing off so the box shows through solid
    // blocks; everything queued before it keeps the depth test it had.
    FlushDebugBatch();
    glDisable(GL_DEPTH_TEST);
    AppendCubeWires(Center, Size.x, Size.y, Size.z, Color);
    FlushDebugBatch();
    glEnable(GL_DEPTH_TEST);
  } else {
    AppendCubeWires(Center, Size.x, Size.y, Size.z, Color);
  }
}

// ---------------------------------------------------------------------------
// UI layer
// ---------------------------------------------------------------------------

int RenderGetMissingBlockTextureCount(void) {
  return GetBackendState()->MissingBlockTextures;
}

// glGetString returns NULL without a current context or on error; callers get
// an empty string instead so they can print it unconditionally.
static const char *GlStringOrEmpty(GLenum Name) {
  const GLubyte *Value = glGetString(Name);
  return (Value != (const GLubyte *)0) ? (const char *)Value : "";
}

const char *RenderGetDeviceName(void) { return GlStringOrEmpty(GL_RENDERER); }

const char *RenderGetApiVersion(void) { return GlStringOrEmpty(GL_VERSION); }

int RenderGetBlockIconGrid(void) {
  int Grid = GetBackendState()->BlockIconGrid;
  return (Grid > 0) ? Grid : MIN_ICON_GRID_DIM;
}

void RenderUploadUiAtlas(const CoreImage *Atlas) {
  BackendState *State = GetBackendState();
  DeleteTexture(&State->UiAtlasTex);
  if (Atlas != NULL && Atlas->Pixels != NULL) {
    State->UiAtlasTex = CreateNearestTexture2D(Atlas);
  }
}

static GLuint UiTextureHandle(UiTextureId Texture) {
  const BackendState *State = GetBackendState();
  return (Texture == UI_TEXTURE_BLOCK_ATLAS) ? State->BlockAtlasTex
                                             : State->UiAtlasTex;
}

enum {
  UI_INITIAL_QUAD_CAPACITY = 4096,
  UI_INDICES_PER_QUAD = 6
};

static void CreateUiVertexArray(BackendState *State) {
  glGenVertexArrays(1, &State->UiVao);
  glGenBuffers(1, &State->UiVbo);
  glGenBuffers(1, &State->UiEbo);

  glBindVertexArray(State->UiVao);
  glBindBuffer(GL_ARRAY_BUFFER, State->UiVbo);
  glEnableVertexAttribArray(UI_ATTRIB_POSITION);
  glVertexAttribPointer(UI_ATTRIB_POSITION, 2, GL_FLOAT, GL_FALSE,
                        (GLsizei)sizeof(UiVertex),
                        GlBufferOffset(offsetof(UiVertex, X)));
  glEnableVertexAttribArray(UI_ATTRIB_TEXCOORD);
  glVertexAttribPointer(UI_ATTRIB_TEXCOORD, 2, GL_FLOAT, GL_FALSE,
                        (GLsizei)sizeof(UiVertex),
                        GlBufferOffset(offsetof(UiVertex, U)));
  glEnableVertexAttribArray(UI_ATTRIB_COLOR);
  glVertexAttribPointer(UI_ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                        (GLsizei)sizeof(UiVertex),
                        GlBufferOffset(offsetof(UiVertex, Color)));
  // The element buffer binding is part of the VAO state.
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, State->UiEbo);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// Grows the vertex buffer to hold QuadCount quads. The index buffer only ever
// holds the fixed two-triangle pattern per quad, so it is rebuilt on growth
// and never touched per frame.
static bool EnsureUiCapacity(BackendState *State, int QuadCount) {
  if (State->UiVao == 0) {
    CreateUiVertexArray(State);
  }
  if (QuadCount <= State->UiQuadCapacity) {
    return true;
  }

  int Capacity = (State->UiQuadCapacity > 0) ? State->UiQuadCapacity
                                             : UI_INITIAL_QUAD_CAPACITY;
  while (Capacity < QuadCount) {
    Capacity *= 2;
  }

  size_t IndexCount = (size_t)Capacity * UI_INDICES_PER_QUAD;
  unsigned int *Indices = (unsigned int *)malloc(IndexCount * sizeof(unsigned int));
  if (Indices == NULL) {
    LogWarn("RENDER: out of memory growing the UI index buffer");
    return false;
  }
  static const unsigned int QUAD_PATTERN[UI_INDICES_PER_QUAD] = {0, 1, 2,
                                                                 0, 2, 3};
  for (unsigned int Quad = 0; Quad < (unsigned int)Capacity; Quad++) {
    unsigned int Base = Quad * UI_QUAD_CORNERS;
    unsigned int *Out = &Indices[(size_t)Quad * UI_INDICES_PER_QUAD];
    for (int Corner = 0; Corner < UI_INDICES_PER_QUAD; Corner++) {
      Out[Corner] = Base + QUAD_PATTERN[Corner];
    }
  }

  glBindVertexArray(State->UiVao);
  glBindBuffer(GL_ARRAY_BUFFER, State->UiVbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)Capacity * sizeof(UiQuad)),
               NULL, GL_STREAM_DRAW);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               (GLsizeiptr)(IndexCount * sizeof(unsigned int)), Indices,
               GL_STATIC_DRAW);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  free(Indices);

  State->UiQuadCapacity = Capacity;
  return true;
}

// Converts a top-left scissor rectangle in screen pixels to the framebuffer's
// bottom-left convention, scaling when the framebuffer is not the screen size
// (high DPI).
static void ApplyUiScissor(UiScissor Scissor) {
  int ScreenW = PlatformGetScreenWidth();
  int ScreenH = PlatformGetScreenHeight();
  int RenderW = 0;
  int RenderH = 0;
  PlatformGetFramebufferSize(&RenderW, &RenderH);
  float ScaleX = (ScreenW > 0) ? (float)RenderW / (float)ScreenW : 1.0F;
  float ScaleY = (ScreenH > 0) ? (float)RenderH / (float)ScreenH : 1.0F;
  int X = (int)((float)Scissor.X * ScaleX);
  int W = (int)((float)Scissor.W * ScaleX);
  int H = (int)((float)Scissor.H * ScaleY);
  int Y = RenderH - (int)((float)(Scissor.Y + Scissor.H) * ScaleY);
  glEnable(GL_SCISSOR_TEST);
  glScissor(X, Y, W, H);
}

void RenderSubmitUI(const UiDrawList *List) {
  BackendState *State = GetBackendState();
  FlushDebugBatch();
  if (List->QuadCount <= 0 || State->UiProgram == 0) {
    return;
  }
  if (!EnsureUiCapacity(State, List->QuadCount)) {
    return;
  }

  glUseProgram(State->UiProgram);
  glUniformMatrix4fv(State->UiMvpLoc, 1, GL_FALSE, State->Mvp2D);
  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(State->UiVao);
  glBindBuffer(GL_ARRAY_BUFFER, State->UiVbo);
  // Orphan the previous frame's storage so the upload never waits on a draw
  // the GPU is still reading from.
  glBufferData(GL_ARRAY_BUFFER,
               (GLsizeiptr)((size_t)State->UiQuadCapacity * sizeof(UiQuad)), NULL,
               GL_STREAM_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, 0,
                  (GLsizeiptr)((size_t)List->QuadCount * sizeof(UiQuad)),
                  List->Quads);

  for (int CmdIdx = 0; CmdIdx < List->CommandCount; CmdIdx++) {
    const UiCommand *Cmd = &List->Commands[CmdIdx];
    bool Scissored = Cmd->Scissor.W > 0;
    if (Scissored) {
      ApplyUiScissor(Cmd->Scissor);
    }

    glBindTexture(GL_TEXTURE_2D, UiTextureHandle(Cmd->Texture));
    // Every quad has the same index pattern, so each command reads the indices
    // from the start and shifts them to its first quad with the base vertex.
    glDrawElementsBaseVertex(GL_TRIANGLES, Cmd->QuadCount * UI_INDICES_PER_QUAD,
                             GL_UNSIGNED_INT, GlBufferOffset(0),
                             Cmd->FirstQuad * UI_QUAD_CORNERS);

    if (Scissored) {
      glDisable(GL_SCISSOR_TEST);
    }
  }

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}
