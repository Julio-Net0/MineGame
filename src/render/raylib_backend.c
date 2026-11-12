#include "render/backend.h"
#include "render/rl_compat.h"
#include "core/fileio.h"
#include "core/image.h"
#include "core/log.h"
#include "core/resource_pack.h"
#include "core/profiler.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "world/world.h"
#include "raylib.h"
#include "raymath.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Must include glad before rlgl to get raw GL constants (GL_TEXTURE_2D_ARRAY etc.)
#include "external/glad.h"
#include <rlgl.h>

#define BLOCK_HIGHLIGHT_SCALE (BLOCK_SIZE + 0.01F)

// Three meshes per active chunk at most: solid faces, alpha-cutout flora, and
// translucent blocks. Flora was split out of the solid mesh so the two could use
// fragment shaders that differ on the alpha test.
#define MESHES_PER_CHUNK 3
#define MESH_POOL_CAPACITY (MAX_ACTIVE_CHUNKS * MESHES_PER_CHUNK)

enum {
  CHUNK_VERTEX_ATTRIB_LOC = 0,
  CHUNK_VERTEX_WORDS = 2
};

// GPU-only chunk mesh handle record. Vertex data is uploaded straight to Vbo and
// never retained in system RAM; every chunk Vao binds the single shared static
// EBO populated at startup.
typedef struct {
  unsigned int Vao;
  unsigned int Vbo;
  int IndexCount;
} ChunkGpuMesh;

#ifdef MINEGAME_PROFILE
enum {
  SOLID_QUERY_COUNT = 2
};
#endif

typedef struct {
  Texture2D BlockAtlas;
  Texture2D UiAtlas;

  // UI draw list path: the whole list is copied into one vertex buffer and
  // each command is one indexed draw, instead of three rlgl calls per vertex.
  Shader UiShader;
  int UiMvpLoc;
  int UiTextureLoc;
  unsigned int UiVao;
  unsigned int UiVbo;
  unsigned int UiEbo;
  int UiQuadCapacity;
  unsigned int BlockArrayTexID;

  // Same vertex shader, two fragment shaders differing only by the alpha test.
  Shader ChunkShaderOpaque;
  Shader ChunkShaderCutout;
  // Atlas grid dimension chosen when the block icon atlas was built.
  int BlockIconGrid;
  // Registered block textures that fell back to the checkerboard.
  int MissingBlockTextures;

  int ChunkOriginLocOpaque;
  int ChunkOriginLocCutout;
  int ActiveChunkOriginLoc;
  // Vertex array the current chunk pass last bound, 0 outside a pass. Chunk
  // draws replace the binding directly instead of resetting it to 0 after
  // each one; the pass end releases it, so no buffer bound outside a pass
  // (Raylib's batch flush, mesh uploads) is recorded into a chunk's VAO.
  unsigned int BoundChunkVao;

  unsigned int ChunkSharedEbo;
  ChunkGpuMesh MeshPool[MESH_POOL_CAPACITY];
  bool MeshSlotUsed[MESH_POOL_CAPACITY];
  MeshHandle FreeList[MESH_POOL_CAPACITY];
  int FreeCount;

  // Frame capture: Requested is set while a frame is being built, Capture holds
  // the pixels once that frame has been read back and is waiting to be taken.
  bool CaptureRequested;
  CoreImage Capture;

#ifdef MINEGAME_PROFILE
  // Double-buffered occlusion queries around the solid chunk pass. One slot is
  // written by the current frame while the other, issued at least one frame
  // earlier, is polled with GL_QUERY_RESULT_AVAILABLE so reading it never
  // stalls the pipeline it is measuring.
  unsigned int SolidQueries[SOLID_QUERY_COUNT];
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

// ---------------------------------------------------------------------------
// Texture array and HUD atlas loader
// ---------------------------------------------------------------------------

enum {
  MIN_ICON_GRID_DIM = 16,
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
  // Only a registered name counts: the single placeholder layer an empty
  // registry gets is expected, not missing.
  if (Name != NULL) {
    GetBackendState()->MissingBlockTextures++;
  }
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
                              int TileIdx, int TileSize, int GridDim) {
  int TileCol = TileIdx % GridDim;
  int TileRow = TileIdx / GridDim;
  if (TileRow >= GridDim) {
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
    TraceLog(LOG_WARNING,
             "RENDER: %d block textures registered but the driver supports only "
             "%d array layers; textures past the limit will sample wrong layers",
             LayerCount, (int)MaxLayers);
    return (int)MaxLayers;
  }
  return LayerCount;
}

static void LoadBlockTextureArray(void) {
  BackendState *State = GetBackendState();

  int RegisteredCount = GetBlockTextureCount();
  int LayerCount = (RegisteredCount > 0) ? RegisteredCount : 1;
  if (LayerCount > MAX_BLOCK_TEXTURES) {
    LayerCount = MAX_BLOCK_TEXTURES;
  }
  LayerCount = ClampLayerCountToDriver(LayerCount);

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
  GLint MaxTextureSize = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &MaxTextureSize);
  int GridDim = DeriveIconGridDim(LayerCount);
  int IconTile = DeriveIconTileSize(TileSize, GridDim, (int)MaxTextureSize);
  State->BlockIconGrid = GridDim;
  int AtlasDim = GridDim * IconTile;
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
    if (IconTile != TileSize) {
      ImageResizeNN(&Tiles[Idx], IconTile, IconTile);
    }
    CopyTileIntoAtlas(&AtlasImg, &Tiles[Idx], Idx, IconTile, GridDim);
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

// Every quad in a chunk mesh uses the standard (0, 1, 2, 0, 2, 3) index split
// (AO-flipped quads rotate their four vertices at emit time), so one static
// 16-bit index buffer serves every chunk VAO in the engine.
static void InitSharedChunkIndexBuffer(BackendState *State) {
  unsigned short *Indices = (unsigned short *)malloc(
      (size_t)CHUNK_MAX_INDICES * sizeof(unsigned short));
  if (Indices == NULL) {
    TraceLog(LOG_WARNING,
             "RENDER: out of memory allocating shared chunk index buffer");
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
               (long)((size_t)CHUNK_MAX_INDICES * sizeof(unsigned short)),
               Indices, GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  free(Indices);
}

void RenderBackendInit(void) {
  BackendState *State = GetBackendState();

  LoadBlockTextureArray();

  State->ChunkShaderOpaque = LoadShader("assets/shaders/chunk.vert",
                                        "assets/shaders/chunk_opaque.frag");
  State->ChunkShaderCutout = LoadShader("assets/shaders/chunk.vert",
                                        "assets/shaders/chunk_cutout.frag");
  State->ChunkOriginLocOpaque =
      GetShaderLocation(State->ChunkShaderOpaque, "uChunkOrigin");
  State->ChunkOriginLocCutout =
      GetShaderLocation(State->ChunkShaderCutout, "uChunkOrigin");
  State->ActiveChunkOriginLoc = -1;

  State->UiShader =
      LoadShader("assets/shaders/ui.vert", "assets/shaders/ui.frag");
  State->UiMvpLoc = GetShaderLocation(State->UiShader, "mvp");
  State->UiTextureLoc = GetShaderLocation(State->UiShader, "texture0");

  int Unit = 0;
  SetShaderValue(State->ChunkShaderOpaque,
                 GetShaderLocation(State->ChunkShaderOpaque, "uBlockArray"),
                 &Unit, SHADER_UNIFORM_INT);
  SetShaderValue(State->ChunkShaderCutout,
                 GetShaderLocation(State->ChunkShaderCutout, "uBlockArray"),
                 &Unit, SHADER_UNIFORM_INT);

  // Uniform values persist on the program, so the overlay table is set once
  // here rather than per chunk pass.
  const int *OverlayLayers = GetOverlaySlotLayers();
  SetShaderValueV(State->ChunkShaderOpaque,
                  GetShaderLocation(State->ChunkShaderOpaque, "uOverlayLayers"),
                  OverlayLayers, SHADER_UNIFORM_INT, OVERLAY_SLOT_COUNT);
  SetShaderValueV(State->ChunkShaderCutout,
                  GetShaderLocation(State->ChunkShaderCutout, "uOverlayLayers"),
                  OverlayLayers, SHADER_UNIFORM_INT, OVERLAY_SLOT_COUNT);

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
  UnloadShader(State->ChunkShaderOpaque);
  UnloadShader(State->ChunkShaderCutout);
  UnloadShader(State->UiShader);
  if (State->UiVao != 0) {
    glDeleteVertexArrays(1, &State->UiVao);
    glDeleteBuffers(1, &State->UiVbo);
    glDeleteBuffers(1, &State->UiEbo);
    State->UiVao = 0;
    State->UiQuadCapacity = 0;
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
  if (Data == NULL || Data->Vertices == NULL || Data->VertexCount <= 0) {
    return MESH_HANDLE_INVALID;
  }
  if (State->FreeCount == 0) {
    TraceLog(LOG_WARNING, "RENDER: mesh pool exhausted");
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
               (long)((size_t)Data->VertexCount * sizeof(ChunkVertex)),
               Data->Vertices, GL_STATIC_DRAW);
  glEnableVertexAttribArray(CHUNK_VERTEX_ATTRIB_LOC);
  glVertexAttribIPointer(CHUNK_VERTEX_ATTRIB_LOC, CHUNK_VERTEX_WORDS,
                         GL_UNSIGNED_INT, (int)sizeof(ChunkVertex),
                         (const void *)0);
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
// Custom draw — bypasses Raylib material/texture binding
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
  Shader *Active = (Kind == CHUNK_PASS_CUTOUT) ? &State->ChunkShaderCutout
                                               : &State->ChunkShaderOpaque;
  State->ActiveChunkOriginLoc = (Kind == CHUNK_PASS_CUTOUT)
                                    ? State->ChunkOriginLocCutout
                                    : State->ChunkOriginLocOpaque;

  // Raylib batches its own immediate-mode geometry and flushes it lazily. Left
  // pending, it would be drawn later under this pass's shader.
  rlDrawRenderBatchActive();
  State->BoundChunkVao = 0;

  rlEnableShader(Active->id);

  Matrix MatMVP = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
  rlSetUniformMatrix(Active->locs[SHADER_LOC_MATRIX_MVP], MatMVP);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, State->BlockArrayTexID);

#ifdef MINEGAME_PROFILE
  BeginSolidOcclusionQuery(State);
#endif
}

void RenderEndChunkPass(void) {
  BackendState *State = GetBackendState();
#ifdef MINEGAME_PROFILE
  EndSolidOcclusionQuery(State);
#endif
  State->ActiveChunkOriginLoc = -1;
  if (State->BoundChunkVao != 0) {
    rlDisableVertexArray();
    State->BoundChunkVao = 0;
  }
  rlDisableShader();
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
  int Width = GetRenderWidth();
  int Height = GetRenderHeight();
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
  Matrix M = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());

  // Listed by field name rather than copied over the struct, so the layout is
  // stated here instead of depending on how the backend's matrix type happens to
  // be ordered in memory. Column-major: element[Column * 4 + Row].
  float Source[RENDER_MATRIX_ELEMENTS] = {
      M.m0, M.m1, M.m2,  M.m3,  M.m4,  M.m5,  M.m6,  M.m7,
      M.m8, M.m9, M.m10, M.m11, M.m12, M.m13, M.m14, M.m15};

  memcpy(OutMatrix, Source, sizeof(Source));
}

void RenderDrawMesh(MeshHandle Handle, Vec3 ChunkOrigin) {
  BackendState *State = GetBackendState();
  if (Handle == MESH_HANDLE_INVALID || Handle >= (MeshHandle)MESH_POOL_CAPACITY) {
    return;
  }
  if (!State->MeshSlotUsed[Handle]) {
    return;
  }
  ChunkGpuMesh *MeshVal = &State->MeshPool[Handle];
  if (MeshVal->Vao == 0 || MeshVal->IndexCount <= 0) {
    return;
  }

  if (State->ActiveChunkOriginLoc >= 0) {
    glUniform3f(State->ActiveChunkOriginLoc, ChunkOrigin.x, ChunkOrigin.y,
                ChunkOrigin.z);
  }

  if (State->BoundChunkVao != MeshVal->Vao) {
    rlEnableVertexArray(MeshVal->Vao);
    State->BoundChunkVao = MeshVal->Vao;
  }
  rlDrawVertexArrayElements(0, MeshVal->IndexCount, 0);
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void RenderBeginFrame(RenderCamera Camera) {
  PROFILE_BEGIN(SetupStart);
#ifdef MINEGAME_PROFILE
  GetBackendState()->SolidPassNext = true;
#endif
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

  // The UI is still sitting in rlgl's batch at this point. EndDrawing would
  // flush it, but the read has to happen before the swap, so the flush does
  // too -- otherwise the capture shows the 3D scene without the UI over it.
  rlDrawRenderBatchActive();

  int Width = GetScreenWidth();
  int Height = GetScreenHeight();
  if (Width <= 0 || Height <= 0) {
    return;
  }

  // rlReadScreenPixels already flips the rows glReadPixels returns bottom-up
  // and forces alpha to 255, which is exactly CoreImage's layout.
  unsigned char *Pixels = rlReadScreenPixels(Width, Height);
  if (Pixels == (unsigned char *)0) {
    LogError("RENDER: frame capture failed to read %dx%d pixels", Width, Height);
    return;
  }

  // Copied out of Raylib's allocator into ours: a CoreImage is freed with
  // CoreImageFree wherever it ends up, which must not depend on how the
  // renderer was configured to allocate.
  size_t Bytes = (size_t)Width * (size_t)Height * CORE_IMAGE_CHANNELS;
  unsigned char *Owned = (unsigned char *)malloc(Bytes);
  if (Owned == (unsigned char *)0) {
    LogError("RENDER: out of memory copying a %dx%d frame capture", Width,
             Height);
    MemFree(Pixels);
    return;
  }
  memcpy(Owned, Pixels, Bytes);
  MemFree(Pixels);

  // A capture nobody took yet is replaced, not leaked: the newer frame is the
  // one that was asked for.
  CoreImageFree(&State->Capture);
  State->Capture.Width = Width;
  State->Capture.Height = Height;
  State->Capture.Pixels = Owned;
}

// EndDrawing is the buffer swap plus the platform's input-event pump, so this
// scope covers everything the driver and the OS can make the frame wait on.
// With no vsync and the GPU queue kept full it is the likeliest home of a stall
// that no other scope accounts for.
void RenderEndFrame(void) {
  CaptureFrameIfRequested();

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

int RenderGetMissingBlockTextureCount(void) {
  return GetBackendState()->MissingBlockTextures;
}

int RenderGetBlockIconGrid(void) {
  int Grid = GetBackendState()->BlockIconGrid;
  return (Grid > 0) ? Grid : MIN_ICON_GRID_DIM;
}

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

enum {
  UI_INITIAL_QUAD_CAPACITY = 4096,
  UI_INDICES_PER_QUAD = 6
};

// OpenGL takes a buffer offset through a pointer parameter. The union states
// that conversion once instead of casting an integer to a pointer at each call.
static const void *GlBufferOffset(size_t Offset) {
  union {
    uintptr_t Offset;
    const void *Pointer;
  } Converted = {.Offset = (uintptr_t)Offset};
  return Converted.Pointer;
}

static void BindUiAttribute(unsigned int Program, const char *Name,
                            int Components, unsigned int Type,
                            unsigned char Normalized, size_t Offset) {
  int Location = glGetAttribLocation(Program, Name);
  if (Location < 0) {
    return;
  }
  glEnableVertexAttribArray((unsigned int)Location);
  glVertexAttribPointer((unsigned int)Location, Components, Type, Normalized,
                        (int)sizeof(UiVertex), GlBufferOffset(Offset));
}

static void CreateUiVertexArray(BackendState *State) {
  glGenVertexArrays(1, &State->UiVao);
  glGenBuffers(1, &State->UiVbo);
  glGenBuffers(1, &State->UiEbo);

  glBindVertexArray(State->UiVao);
  glBindBuffer(GL_ARRAY_BUFFER, State->UiVbo);
  unsigned int Program = State->UiShader.id;
  BindUiAttribute(Program, "vertexPosition", 2, GL_FLOAT, GL_FALSE,
                  offsetof(UiVertex, X));
  BindUiAttribute(Program, "vertexTexCoord", 2, GL_FLOAT, GL_FALSE,
                  offsetof(UiVertex, U));
  BindUiAttribute(Program, "vertexColor", 4, GL_UNSIGNED_BYTE, GL_TRUE,
                  offsetof(UiVertex, Color));
  // The element buffer binding is part of the VAO state.
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, State->UiEbo);
  glBindVertexArray(0);
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
    TraceLog(LOG_WARNING, "RENDER: out of memory growing the UI index buffer");
    return false;
  }
  // Same split as rlgl's quads, so the winding matches its back-face culling.
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
  glBufferData(GL_ARRAY_BUFFER, (long)((size_t)Capacity * sizeof(UiQuad)), NULL,
               GL_STREAM_DRAW);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               (long)(IndexCount * sizeof(unsigned int)), Indices,
               GL_STATIC_DRAW);
  glBindVertexArray(0);
  free(Indices);

  State->UiQuadCapacity = Capacity;
  return true;
}

// Converts a top-left scissor rectangle in screen pixels to the framebuffer's
// bottom-left convention, scaling when the framebuffer is not the screen size
// (high-DPI). Raylib's BeginScissorMode is not usable here: it flushes the
// rlgl batch, and that flush unbinds the current program and VAO even when the
// batch is empty, which left every scissored UI command drawing nothing.
static void ApplyUiScissor(UiScissor Scissor) {
  int ScreenW = GetScreenWidth();
  int ScreenH = GetScreenHeight();
  float ScaleX = (ScreenW > 0) ? (float)GetRenderWidth() / (float)ScreenW : 1.0F;
  float ScaleY = (ScreenH > 0) ? (float)GetRenderHeight() / (float)ScreenH : 1.0F;
  int X = (int)((float)Scissor.X * ScaleX);
  int W = (int)((float)Scissor.W * ScaleX);
  int H = (int)((float)Scissor.H * ScaleY);
  int Y = GetRenderHeight() - (int)((float)(Scissor.Y + Scissor.H) * ScaleY);
  glEnable(GL_SCISSOR_TEST);
  glScissor(X, Y, W, H);
}

void RenderSubmitUI(const UiDrawList *List) {
  BackendState *State = GetBackendState();
  if (List->QuadCount <= 0 || State->UiShader.id == 0) {
    return;
  }
  // Anything Raylib still has batched (debug lines, the 3D pass) must reach
  // the GPU first, or it would draw over the UI.
  rlDrawRenderBatchActive();
  if (!EnsureUiCapacity(State, List->QuadCount)) {
    return;
  }

  // Raylib's own 2D projection for this frame, so the UI lands exactly where
  // its immediate helpers would have drawn it.
  Matrix Mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());

  glUseProgram(State->UiShader.id);
  rlSetUniformMatrix(State->UiMvpLoc, Mvp);
  glUniform1i(State->UiTextureLoc, 0);
  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(State->UiVao);
  glBindBuffer(GL_ARRAY_BUFFER, State->UiVbo);
  // Orphan the previous frame's storage so the upload never waits on a draw
  // the GPU is still reading from.
  glBufferData(GL_ARRAY_BUFFER,
               (long)((size_t)State->UiQuadCapacity * sizeof(UiQuad)), NULL,
               GL_STREAM_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, 0,
                  (long)((size_t)List->QuadCount * sizeof(UiQuad)),
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
                             GL_UNSIGNED_INT, NULL,
                             Cmd->FirstQuad * UI_QUAD_CORNERS);

    if (Scissored) {
      glDisable(GL_SCISSOR_TEST);
    }
  }

  // rlgl rebinds its own program, VAO and texture on every batch it draws, so
  // clearing ours is enough to hand the context back.
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

// ---------------------------------------------------------------------------
// 3D debug wireframe
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
