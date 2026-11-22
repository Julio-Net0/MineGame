#include "render/gl_lod.h"

#include "core/log.h"
#include "render/gl_internal.h"
#include "render/gl_math.h"
#include "world/block_system.h"

#include <stdlib.h>
#include <string.h>

enum {
  // Arena: one vertex buffer in pages of 256 vertices, so every section mesh
  // shares one vertex array and a whole pass is one multi-draw.
  LOD_PAGE_SHIFT = 8,
  LOD_PAGE_VERTICES = 1 << LOD_PAGE_SHIFT,
  LOD_ARENA_PAGES = 16384,
  LOD_MAX_MESHES = 65536,
  LOD_MAX_DRAWS = 32768,
  // A freed run stays reserved this many presented frames, so no upload can
  // overwrite vertices a frame still queued on the GPU reads.
  LOD_RELEASE_DELAY_FRAMES = 3,
  LOD_META_COMPONENTS = 4,
  LOD_INDICES_PER_QUAD = 6,
  LOD_VERTICES_PER_QUAD = 4,
  BLOCK_UNIT = 0,
  LOD_COLOR_UNIT = 1,
  DEPTH_NEAR_UNIT = 2,
  COVERAGE_UNIT = 3,
  ATTRIB_VERTEX = 0,
  ATTRIB_SECTION = 1,
  FULLSCREEN_VERTICES = 3,
  // GL errors seen in the first LOD frames are recorded; later ones are not
  // polled, since asking every frame can stall the pipeline.
  ERROR_CHECK_FRAMES = 8,
  PENDING_INITIAL = 64,
  MAT4_FLOATS = 16,
  BYTES_TO_MB_SHIFT = 20
};

// The LOD projection: its near plane well inside the 1:1 disc and its far
// plane past the largest view distance (2048 chunks = 32768 blocks).
#define LOD_NEAR_PLANE 8.0
#define LOD_FAR_PLANE 40000.0

typedef struct {
  GLuint Count;
  GLuint InstanceCount;
  GLuint FirstIndex;
  GLint BaseVertex;
  GLuint BaseInstance;
} LodIndirectCommand;

typedef struct {
  int FirstPage;
  int PageCount;
  int VertexCount;
  bool Used;
} LodMeshRecord;

typedef struct {
  int FirstPage;
  int PageCount;
  uint64_t Frame;
} LodPendingRun;

typedef struct {
  GLuint Program;
  GLint ViewProj;
  GLint CameraBlock;
  GLint CameraFrac;
} LodProgram;

typedef struct {
  bool Active;
  GLuint SharedIndexBuffer;

  GLuint Vbo;
  GLuint Vao;
  GLuint InstanceVbo;
  GLuint MetaSsbo;
  GLuint IndirectBuffer;
  GLuint CoverageTex;

  uint8_t *PageUsed;
  int UsedPages;
  int Cursor;
  LodMeshRecord *Meshes;
  int *FreeMeshes;
  int FreeMeshCount;
  int LiveMeshes;
  LodPendingRun *Pending;
  int PendingCount;
  int PendingCapacity;
  uint64_t Frame;

  // Copy of the default framebuffer's depth, the remap's input.
  GLuint DepthCopyTex;
  GLuint FboCopy;
  // The LOD pass's own colour (zero alpha where it drew nothing) and depth.
  GLuint LodColorTex;
  GLuint DepthLodRbo;
  GLuint FboLod;
  int Width;
  int Height;

  GLuint CompositeProgram;
  GLuint RemapProgram;
  GLuint RemapVao;
  GLint RemapNear;
  GLint RemapLod;
  LodProgram Opaque;
  LodProgram Border;

  int32_t *Meta;
  LodIndirectCommand *Commands;

  RenderLodStats Stats;
  int CheckedFrames;
} GlLodState;

static GlLodState *GetLodState(void) {
  static GlLodState State;
  return &State;
}

bool GlLodActive(void) { return GetLodState()->Active; }

static const void *BufferOffset(size_t Offset) {
  union {
    uintptr_t Offset;
    const void *Pointer;
  } Converted = {.Offset = (uintptr_t)Offset};
  return Converted.Pointer;
}

static LodProgram LoadLodProgram(const char *FragmentPath) {
  LodProgram Result = {0};
  Result.Program = GlLinkShaderProgram("assets/shaders/lod.vert", FragmentPath);
  Result.ViewProj = glGetUniformLocation(Result.Program, "uViewProj");
  Result.CameraBlock = glGetUniformLocation(Result.Program, "uCameraBlock");
  Result.CameraFrac = glGetUniformLocation(Result.Program, "uCameraFrac");
  glUseProgram(Result.Program);
  glUniform1i(glGetUniformLocation(Result.Program, "uBlockArray"), BLOCK_UNIT);
  glUniform1i(glGetUniformLocation(Result.Program, "uCoverage"), COVERAGE_UNIT);
  glUniform1iv(glGetUniformLocation(Result.Program, "uOverlayLayers"),
               OVERLAY_SLOT_COUNT, GetOverlaySlotLayers());
  glUseProgram(0);
  return Result;
}

static void DeleteTargets(GlLodState *State) {
  GLuint Framebuffers[2] = {State->FboCopy, State->FboLod};
  for (int Idx = 0; Idx < 2; Idx++) {
    if (Framebuffers[Idx] != 0) {
      glDeleteFramebuffers(1, &Framebuffers[Idx]);
    }
  }
  if (State->DepthLodRbo != 0) {
    glDeleteRenderbuffers(1, &State->DepthLodRbo);
  }
  GLuint Textures[2] = {State->DepthCopyTex, State->LodColorTex};
  for (int Idx = 0; Idx < 2; Idx++) {
    if (Textures[Idx] != 0) {
      glDeleteTextures(1, &Textures[Idx]);
    }
  }
  State->FboCopy = 0;
  State->FboLod = 0;
  State->DepthLodRbo = 0;
  State->DepthCopyTex = 0;
  State->LodColorTex = 0;
  State->Width = 0;
  State->Height = 0;
}

static GLuint CreateTargetTexture(GLenum Internal, GLenum Format, GLenum Type,
                                  int Width, int Height) {
  GLuint Texture = 0;
  glGenTextures(1, &Texture);
  glBindTexture(GL_TEXTURE_2D, Texture);
  glTexImage2D(GL_TEXTURE_2D, 0, (GLint)Internal, Width, Height, 0, Format,
               Type, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  return Texture;
}

// The depth copy matches the default framebuffer's 24/8 depth-stencil, which
// a depth blit requires; the LOD depth is 32-bit float and never sampled.
static bool CreateTargets(GlLodState *State, int Width, int Height) {
  DeleteTargets(State);
  State->DepthCopyTex = CreateTargetTexture(
      GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, Width, Height);
  State->LodColorTex =
      CreateTargetTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, Width, Height);
  glGenRenderbuffers(1, &State->DepthLodRbo);
  glBindRenderbuffer(GL_RENDERBUFFER, State->DepthLodRbo);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, Width, Height);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  glGenFramebuffers(1, &State->FboCopy);
  glBindFramebuffer(GL_FRAMEBUFFER, State->FboCopy);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                         GL_TEXTURE_2D, State->DepthCopyTex, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  bool CopyComplete =
      glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

  glGenFramebuffers(1, &State->FboLod);
  glBindFramebuffer(GL_FRAMEBUFFER, State->FboLod);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         State->LodColorTex, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, State->DepthLodRbo);
  bool LodComplete =
      glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (!CopyComplete || !LodComplete) {
    LogWarn("RENDER: LOD framebuffers incomplete at %dx%d", Width, Height);
    DeleteTargets(State);
    return false;
  }
  State->Width = Width;
  State->Height = Height;
  return true;
}

static bool CreateArena(GlLodState *State) {
  size_t VertexBytes = (size_t)LOD_ARENA_PAGES * LOD_PAGE_VERTICES * sizeof(ChunkVertex);
  glGenBuffers(1, &State->Vbo);
  glBindBuffer(GL_ARRAY_BUFFER, State->Vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)VertexBytes, NULL, GL_DYNAMIC_DRAW);

  // Identity: draw i reads section record i through its baseInstance.
  uint32_t *Ids = (uint32_t *)malloc(sizeof(uint32_t) * LOD_MAX_DRAWS);
  if (Ids == NULL) {
    return false;
  }
  for (int Idx = 0; Idx < LOD_MAX_DRAWS; Idx++) {
    Ids[Idx] = (uint32_t)Idx;
  }
  glGenBuffers(1, &State->InstanceVbo);
  glBindBuffer(GL_ARRAY_BUFFER, State->InstanceVbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * LOD_MAX_DRAWS),
               Ids, GL_STATIC_DRAW);
  free(Ids);

  glGenVertexArrays(1, &State->Vao);
  glBindVertexArray(State->Vao);
  glBindBuffer(GL_ARRAY_BUFFER, State->Vbo);
  glEnableVertexAttribArray(ATTRIB_VERTEX);
  glVertexAttribIPointer(ATTRIB_VERTEX, 2, GL_UNSIGNED_INT,
                         (GLsizei)sizeof(ChunkVertex), BufferOffset(0));
  glBindBuffer(GL_ARRAY_BUFFER, State->InstanceVbo);
  glEnableVertexAttribArray(ATTRIB_SECTION);
  glVertexAttribIPointer(ATTRIB_SECTION, 1, GL_UNSIGNED_INT,
                         (GLsizei)sizeof(uint32_t), BufferOffset(0));
  glVertexAttribDivisor(ATTRIB_SECTION, 1);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, State->SharedIndexBuffer);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  glGenBuffers(1, &State->MetaSsbo);
  glGenBuffers(1, &State->IndirectBuffer);
  glGenVertexArrays(1, &State->RemapVao);
  return true;
}

static bool CreateCoverage(GlLodState *State) {
  size_t Words = (size_t)RENDER_LOD_COVERAGE_SPAN * RENDER_LOD_COVERAGE_SPAN *
                 RENDER_LOD_COVERAGE_WORDS;
  uint32_t *Zero = (uint32_t *)calloc(Words, sizeof(uint32_t));
  if (Zero == NULL) {
    return false;
  }
  glGenTextures(1, &State->CoverageTex);
  glBindTexture(GL_TEXTURE_2D, State->CoverageTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, RENDER_LOD_COVERAGE_SPAN,
               RENDER_LOD_COVERAGE_SPAN, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, Zero);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  free(Zero);
  return true;
}

bool GlLodInit(GLuint SharedIndexBuffer) {
  GlLodState *State = GetLodState();
  *State = (GlLodState){0};
  State->SharedIndexBuffer = SharedIndexBuffer;
  State->PageUsed = (uint8_t *)calloc(LOD_ARENA_PAGES, sizeof(uint8_t));
  State->Meshes = (LodMeshRecord *)calloc(LOD_MAX_MESHES, sizeof(LodMeshRecord));
  State->FreeMeshes = (int *)malloc(sizeof(int) * LOD_MAX_MESHES);
  State->Meta = (int32_t *)malloc(sizeof(int32_t) * LOD_META_COMPONENTS *
                                  LOD_MAX_DRAWS);
  State->Commands =
      (LodIndirectCommand *)malloc(sizeof(LodIndirectCommand) * LOD_MAX_DRAWS);
  if (State->PageUsed == NULL || State->Meshes == NULL ||
      State->FreeMeshes == NULL || State->Meta == NULL ||
      State->Commands == NULL || !CreateArena(State) || !CreateCoverage(State)) {
    GlLodShutdown();
    LogWarn("RENDER: could not create the LOD resources; LOD stays off");
    return false;
  }
  for (int Idx = 0; Idx < LOD_MAX_MESHES; Idx++) {
    State->FreeMeshes[Idx] = LOD_MAX_MESHES - 1 - Idx;
  }
  State->FreeMeshCount = LOD_MAX_MESHES;

  State->CompositeProgram = GlLinkShaderProgram(
      "assets/shaders/depth_remap.vert", "assets/shaders/lod_composite.frag");
  glUseProgram(State->CompositeProgram);
  glUniform1i(glGetUniformLocation(State->CompositeProgram, "uLodColor"),
              LOD_COLOR_UNIT);
  glUseProgram(0);
  State->RemapProgram = GlLinkShaderProgram("assets/shaders/depth_remap.vert",
                                            "assets/shaders/depth_remap.frag");
  State->RemapNear = glGetUniformLocation(State->RemapProgram, "uNear");
  State->RemapLod = glGetUniformLocation(State->RemapProgram, "uLod");
  glUseProgram(State->RemapProgram);
  glUniform1i(glGetUniformLocation(State->RemapProgram, "uDepthNear"),
              DEPTH_NEAR_UNIT);
  glUseProgram(0);
  State->Opaque = LoadLodProgram("assets/shaders/lod_opaque.frag");
  State->Border = LoadLodProgram("assets/shaders/lod_border.frag");

  State->Stats.ArenaCapacityBytes =
      (int64_t)LOD_ARENA_PAGES * LOD_PAGE_VERTICES * (int64_t)sizeof(ChunkVertex);
  State->Active = true;
  LogInfo("RENDER: LOD pipeline on: %d-page arena (%lld MB), projection %.0f..%.0f, "
          "standard depth (32-bit float LOD depth), 1:1 left in the default "
          "framebuffer",
          LOD_ARENA_PAGES,
          (long long)(State->Stats.ArenaCapacityBytes >> BYTES_TO_MB_SHIFT), LOD_NEAR_PLANE,
          LOD_FAR_PLANE);
  return true;
}

static void DeleteBuffer(GLuint *Buffer) {
  if (*Buffer != 0) {
    glDeleteBuffers(1, Buffer);
    *Buffer = 0;
  }
}

void GlLodShutdown(void) {
  GlLodState *State = GetLodState();
  DeleteTargets(State);
  DeleteBuffer(&State->Vbo);
  DeleteBuffer(&State->InstanceVbo);
  DeleteBuffer(&State->MetaSsbo);
  DeleteBuffer(&State->IndirectBuffer);
  if (State->Vao != 0) {
    glDeleteVertexArrays(1, &State->Vao);
  }
  if (State->RemapVao != 0) {
    glDeleteVertexArrays(1, &State->RemapVao);
  }
  if (State->CoverageTex != 0) {
    glDeleteTextures(1, &State->CoverageTex);
  }
  GLuint Programs[4] = {State->RemapProgram, State->CompositeProgram,
                        State->Opaque.Program, State->Border.Program};
  for (int Idx = 0; Idx < 4; Idx++) {
    if (Programs[Idx] != 0) {
      glDeleteProgram(Programs[Idx]);
    }
  }
  free(State->PageUsed);
  free(State->Meshes);
  free(State->FreeMeshes);
  free(State->Pending);
  free(State->Meta);
  free(State->Commands);
  *State = (GlLodState){0};
}

// --- arena -------------------------------------------------------------------

static int FindFreeRun(GlLodState *State, int Pages) {
  for (int Pass = 0; Pass < 2; Pass++) {
    int Start = (Pass == 0) ? State->Cursor : 0;
    int End = (Pass == 0) ? LOD_ARENA_PAGES : State->Cursor;
    int Run = 0;
    for (int Page = Start; Page < End; Page++) {
      Run = (State->PageUsed[Page] == 0U) ? Run + 1 : 0;
      if (Run == Pages) {
        return Page - Pages + 1;
      }
    }
  }
  return -1;
}

static void MarkPages(GlLodState *State, int First, int Count, uint8_t Value) {
  memset(State->PageUsed + First, Value, (size_t)Count);
  State->UsedPages += (Value != 0U) ? Count : -Count;
}

RenderLodMesh GlLodUploadMesh(const ChunkVertex *Vertices, int VertexCount) {
  GlLodState *State = GetLodState();
  if (!State->Active || VertexCount <= 0 || State->FreeMeshCount == 0) {
    return RENDER_LOD_MESH_INVALID;
  }
  int Pages = (VertexCount + LOD_PAGE_VERTICES - 1) >> LOD_PAGE_SHIFT;
  int First = FindFreeRun(State, Pages);
  if (First < 0) {
    return RENDER_LOD_MESH_INVALID;
  }
  MarkPages(State, First, Pages, 1U);
  State->Cursor = (First + Pages) % LOD_ARENA_PAGES;
  glBindBuffer(GL_ARRAY_BUFFER, State->Vbo);
  glBufferSubData(GL_ARRAY_BUFFER,
                  (GLintptr)((size_t)First * LOD_PAGE_VERTICES * sizeof(ChunkVertex)),
                  (GLsizeiptr)((size_t)VertexCount * sizeof(ChunkVertex)), Vertices);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  int Mesh = State->FreeMeshes[--State->FreeMeshCount];
  State->Meshes[Mesh] = (LodMeshRecord){
      .FirstPage = First, .PageCount = Pages, .VertexCount = VertexCount, .Used = true};
  State->LiveMeshes++;
  return Mesh;
}

void GlLodFreeMesh(RenderLodMesh Mesh) {
  GlLodState *State = GetLodState();
  if (Mesh < 0 || Mesh >= LOD_MAX_MESHES || !State->Meshes[Mesh].Used) {
    return;
  }
  LodMeshRecord *Record = &State->Meshes[Mesh];
  if (State->PendingCount == State->PendingCapacity) {
    int Capacity = (State->PendingCapacity > 0) ? State->PendingCapacity * 2 : PENDING_INITIAL;
    LodPendingRun *Grown = (LodPendingRun *)realloc(
        State->Pending, (size_t)Capacity * sizeof(LodPendingRun));
    if (Grown == NULL) {
      // Better a leaked run than vertices overwritten mid-frame.
      Record->Used = false;
      State->FreeMeshes[State->FreeMeshCount++] = Mesh;
      State->LiveMeshes--;
      return;
    }
    State->Pending = Grown;
    State->PendingCapacity = Capacity;
  }
  State->Pending[State->PendingCount++] = (LodPendingRun){
      .FirstPage = Record->FirstPage, .PageCount = Record->PageCount,
      .Frame = State->Frame};
  Record->Used = false;
  State->FreeMeshes[State->FreeMeshCount++] = Mesh;
  State->LiveMeshes--;
}

void GlLodEndFrame(void) {
  GlLodState *State = GetLodState();
  if (!State->Active) {
    return;
  }
  State->Frame++;
  int Kept = 0;
  for (int Idx = 0; Idx < State->PendingCount; Idx++) {
    LodPendingRun Run = State->Pending[Idx];
    if (State->Frame - Run.Frame >= LOD_RELEASE_DELAY_FRAMES) {
      MarkPages(State, Run.FirstPage, Run.PageCount, 0U);
    } else {
      State->Pending[Kept++] = Run;
    }
  }
  State->PendingCount = Kept;
}

void GlLodSetCoverage(int ColumnX, int ColumnZ,
                      const uint32_t Words[RENDER_LOD_COVERAGE_WORDS]) {
  GlLodState *State = GetLodState();
  if (!State->Active) {
    return;
  }
  int TexelX = ColumnX & (RENDER_LOD_COVERAGE_SPAN - 1);
  int TexelZ = ColumnZ & (RENDER_LOD_COVERAGE_SPAN - 1);
  glBindTexture(GL_TEXTURE_2D, State->CoverageTex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, TexelX, TexelZ, 1, 1, GL_RGBA_INTEGER,
                  GL_UNSIGNED_INT, Words);
  glBindTexture(GL_TEXTURE_2D, 0);
}

// --- drawing -------------------------------------------------------------------

static void UseLodProgram(const LodProgram *Program, const float ViewProj[MAT4_FLOATS],
                          const int CameraBlock[3], const float CameraFrac[3]) {
  glUseProgram(Program->Program);
  glUniformMatrix4fv(Program->ViewProj, 1, GL_FALSE, ViewProj);
  glUniform3i(Program->CameraBlock, CameraBlock[0], CameraBlock[1], CameraBlock[2]);
  glUniform3f(Program->CameraFrac, CameraFrac[0], CameraFrac[1], CameraFrac[2]);
}

// The default framebuffer's depth, copied and rewritten into the LOD
// projection in the LOD depth buffer; the LOD colour is cleared to zero alpha.
static void RemapDepth(const GlLodState *State, float LodM10, float LodM14,
                       const GlLodFrame *Frame) {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, State->FboCopy);
  glBlitFramebuffer(0, 0, Frame->Width, Frame->Height, 0, 0, Frame->Width,
                    Frame->Height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, State->FboLod);
  glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDepthFunc(GL_ALWAYS);
  glDepthMask(GL_TRUE);
  glDisable(GL_CULL_FACE);
  glUseProgram(State->RemapProgram);
  glUniform2f(State->RemapNear, Frame->NearM10, Frame->NearM14);
  glUniform2f(State->RemapLod, LodM10, LodM14);
  glActiveTexture(GL_TEXTURE0 + DEPTH_NEAR_UNIT);
  glBindTexture(GL_TEXTURE_2D, State->DepthCopyTex);
  glBindVertexArray(State->RemapVao);
  glDrawArrays(GL_TRIANGLES, 0, FULLSCREEN_VERTICES);
  glBindTexture(GL_TEXTURE_2D, 0);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthFunc(GL_LEQUAL);
  glEnable(GL_CULL_FACE);
}

// Copies the LOD's pixels onto the default framebuffer and restores the
// frame's state for the passes after it.
static void CompositeOntoFrame(const GlLodState *State, const GlLodFrame *Frame) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glUseProgram(State->CompositeProgram);
  glActiveTexture(GL_TEXTURE0 + LOD_COLOR_UNIT);
  glBindTexture(GL_TEXTURE_2D, State->LodColorTex);
  glBindVertexArray(State->RemapVao);
  glDrawArrays(GL_TRIANGLES, 0, FULLSCREEN_VERTICES);
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0 + BLOCK_UNIT);
  glUseProgram(0);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glEnable(GL_BLEND);
  glEnable(GL_CULL_FACE);
}

// Fills the section records and indirect commands; returns the vertices drawn.
static int64_t BuildCommands(GlLodState *State, const RenderLodDraw *Draws,
                             int Count) {
  int64_t Vertices = 0;
  for (int Idx = 0; Idx < Count; Idx++) {
    const RenderLodDraw *Draw = &Draws[Idx];
    const LodMeshRecord *Record = &State->Meshes[Draw->Mesh];
    int32_t *Meta = &State->Meta[(size_t)Idx * LOD_META_COMPONENTS];
    Meta[0] = Draw->OriginX;
    Meta[1] = Draw->OriginY;
    Meta[2] = Draw->OriginZ;
    Meta[3] = Draw->Scale;
    State->Commands[Idx] = (LodIndirectCommand){
        .Count = (GLuint)((Record->VertexCount / LOD_VERTICES_PER_QUAD) *
                          LOD_INDICES_PER_QUAD),
        .InstanceCount = 1,
        .FirstIndex = 0,
        .BaseVertex = Record->FirstPage * LOD_PAGE_VERTICES,
        .BaseInstance = (GLuint)Idx};
    Vertices += Record->VertexCount;
  }
  return Vertices;
}

void GlLodDraw(const GlLodFrame *Frame, const RenderLodDraw *Draws,
               int OpaqueCount, int BorderCount) {
  GlLodState *State = GetLodState();
  State->Stats.OpaqueDraws = 0;
  State->Stats.BorderDraws = 0;
  State->Stats.DrawnVertices = 0;
  int Total = OpaqueCount + BorderCount;
  if (!State->Active || Total <= 0) {
    return;
  }
  if ((State->Width != Frame->Width || State->Height != Frame->Height) &&
      !CreateTargets(State, Frame->Width, Frame->Height)) {
    return;
  }
  if (Total > LOD_MAX_DRAWS) {
    BorderCount = (BorderCount > Total - LOD_MAX_DRAWS)
                      ? BorderCount - (Total - LOD_MAX_DRAWS)
                      : 0;
    OpaqueCount = LOD_MAX_DRAWS - BorderCount;
    Total = LOD_MAX_DRAWS;
  }

  // Camera at the origin; the camera's block goes to the shader as an int.
  Vec3 Position = Frame->Camera.Position;
  float FloorX = floorf(Position.x);
  float FloorY = floorf(Position.y);
  float FloorZ = floorf(Position.z);
  int CameraBlock[3] = {(int)FloorX, (int)FloorY, (int)FloorZ};
  float CameraFrac[3] = {Position.x - FloorX, Position.y - FloorY,
                         Position.z - FloorZ};
  Vec3 Forward = {Frame->Camera.Target.x - Position.x,
                  Frame->Camera.Target.y - Position.y,
                  Frame->Camera.Target.z - Position.z};
  float Aspect = (float)Frame->Width / (float)Frame->Height;
  double Top = LOD_NEAR_PLANE *
               tan(Frame->Camera.FovY * GL_MATH_HALF_ANGLE * GL_MATH_DEG2RAD_F);
  double Right = Top * Aspect;
  GlMat4 Projection =
      GlMat4Frustum(-Right, Right, -Top, Top, LOD_NEAR_PLANE, LOD_FAR_PLANE);
  GlMat4 View = GlMat4LookAt((Vec3){0.0F, 0.0F, 0.0F}, Forward, Frame->Camera.Up);
  float ViewProj[GL_MAT4_ELEMENTS];
  GlMat4ToColumnMajor(GlMat4Multiply(View, Projection), ViewProj);

  // The LOD is always drawn filled. Under the wireframe debug view its edges
  // would land exactly on chunk-column borders, where the border shader's
  // coverage lookup cannot tell which column a line belongs to.
  if (Frame->Wireframe) {
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  }
  RemapDepth(State, Projection.m10, Projection.m14, Frame);

  State->Stats.DrawnVertices = BuildCommands(State, Draws, Total);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, State->MetaSsbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER,
               (GLsizeiptr)(sizeof(int32_t) * LOD_META_COMPONENTS * (size_t)Total),
               State->Meta, GL_STREAM_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, State->MetaSsbo);
  glBindBuffer(GL_DRAW_INDIRECT_BUFFER, State->IndirectBuffer);
  glBufferData(GL_DRAW_INDIRECT_BUFFER,
               (GLsizeiptr)(sizeof(LodIndirectCommand) * (size_t)Total),
               State->Commands, GL_STREAM_DRAW);

  glBindVertexArray(State->Vao);
  glActiveTexture(GL_TEXTURE0 + BLOCK_UNIT);
  glBindTexture(GL_TEXTURE_2D_ARRAY, Frame->BlockArrayTex);
  glBindSampler(BLOCK_UNIT, Frame->LodSampler);
  glActiveTexture(GL_TEXTURE0 + COVERAGE_UNIT);
  glBindTexture(GL_TEXTURE_2D, State->CoverageTex);

  if (OpaqueCount > 0) {
    UseLodProgram(&State->Opaque, ViewProj, CameraBlock, CameraFrac);
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, BufferOffset(0),
                                OpaqueCount, 0);
  }
  if (BorderCount > 0) {
    UseLodProgram(&State->Border, ViewProj, CameraBlock, CameraFrac);
    glMultiDrawElementsIndirect(
        GL_TRIANGLES, GL_UNSIGNED_SHORT,
        BufferOffset((size_t)OpaqueCount * sizeof(LodIndirectCommand)),
        BorderCount, 0);
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0 + BLOCK_UNIT);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  glBindSampler(BLOCK_UNIT, 0);
  glBindVertexArray(0);
  glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
  CompositeOntoFrame(State, Frame);
  if (Frame->Wireframe) {
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  }

  State->Stats.OpaqueDraws = OpaqueCount;
  State->Stats.BorderDraws = BorderCount;
  if (State->CheckedFrames < ERROR_CHECK_FRAMES) {
    State->CheckedFrames++;
    GLenum Error = glGetError();
    if (Error != GL_NO_ERROR && State->Stats.GlError == 0U) {
      State->Stats.GlError = (unsigned)Error;
      LogWarn("RENDER: GL error 0x%04X after the LOD pass", (unsigned)Error);
    }
  }
}

void GlLodGetStats(RenderLodStats *Out) {
  const GlLodState *State = GetLodState();
  *Out = State->Stats;
  Out->Meshes = State->LiveMeshes;
  Out->ArenaUsedBytes =
      (int64_t)State->UsedPages * LOD_PAGE_VERTICES * (int64_t)sizeof(ChunkVertex);
}
