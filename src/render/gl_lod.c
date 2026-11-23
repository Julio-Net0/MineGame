#include "render/gl_lod.h"

#include "core/log.h"
#include "render/gl_caps.h"
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
  // Every mesh holds at least one page, so no more can be live at once.
  LOD_MAX_MESHES = LOD_ARENA_PAGES,
  LIST_INITIAL_CAPACITY = 1024,
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
  BYTES_TO_MB_SHIFT = 20,
  // Cull: per section one command slot per face run (the most it can need)
  // and a record of its first vertex, run count, first slot and packed runs
  // (see lod_cull.comp, whose constants must match).
  LOD_RUNS = RENDER_LOD_MAX_RUNS,
  CULL_RECORD_HEADER = 4,
  CULL_RECORD_UINTS = CULL_RECORD_HEADER + LOD_RUNS,
  CULL_RUN_GROUP_SHIFT = 16,
  CULL_WORKGROUP = 64,
  // Counters (see lod_cull.comp, whose indices must match): the opaque and
  // border draw counts of phase 1 (or the single pass) and of phase 2, then
  // statistics.
  COUNTER_OPAQUE = 0,
  COUNTER_BORDER = 1,
  COUNTER_PHASE2_OPAQUE = 2,
  COUNTER_PHASE2_BORDER = 3,
  COUNTER_FRUSTUM_CULLED = 4,
  COUNTER_CANDIDATE_RUNS = 5,
  COUNTER_FACING_CULLED = 6,
  COUNTER_PHASE1_COMMANDS = 7,
  COUNTER_PHASE1_QUADS = 8,
  COUNTER_PHASE2_COMMANDS = 9,
  COUNTER_PHASE2_QUADS = 10,
  COUNTER_HIZ_CULLED = 11,
  COUNTER_UINTS = 16,
  BINDING_META = 0,
  BINDING_CULL = 1,
  BINDING_COMMANDS = 2,
  BINDING_COUNTERS = 3,
  BINDING_VISIBILITY = 4,
  // uPhase in lod_cull.comp.
  PHASE_SINGLE = 0,
  PHASE_FIRST = 1,
  PHASE_SECOND = 2,
  // Hi-Z: levels 1..HIZ_MAX_LEVELS-1 in one R32F atlas (see lod_hiz.comp).
  HIZ_MAX_LEVELS = 16,
  HIZ_LEVEL_FIELDS = 4,
  // lod_hiz.comp: a 16x16 workgroup writes five levels from one source level.
  HIZ_WORKGROUP = 16,
  HIZ_LEVELS_PER_DISPATCH = 5,
  HIZ_IMAGE_UNIT = 0
};

// ARB_indirect_parameters / GL 4.6 tokens, absent from the 4.3 loader.
#define LOD_PARAMETER_BUFFER 0x80EEU
#define LOD_PARAMETER_BUFFER_BARRIER_BIT 0x00001000U

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
  int RunCount;
  // Group << CULL_RUN_GROUP_SHIFT | quads, per run.
  uint32_t Runs[LOD_RUNS];
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
  GLuint Program;
  GLint ViewProj;
  GLint CameraBlock;
  GLint CameraFrac;
  GLint OpaqueCount;
  GLint OpaqueRuns;
  GLint Total;
  GLint Compact;
  GLint CullEnabled;
  GLint Phase;
  GLint PhaseBase;
  GLint Viewport;
  GLint HizTop;
  GLint HizLevels;
} CullProgram;

typedef struct {
  GLuint Program;
  GLint Source;
  GLint Top;
  GLint Levels;
} HizProgram;

typedef struct {
  bool Active;
  bool UseIndirectCount;
  bool CullEnabled;
  bool HizEnabled;
  GLuint SharedIndexBuffer;

  GLuint Vbo;
  GLuint Vao;
  GLuint InstanceVbo;
  GLuint MetaSsbo;
  GLuint CullSsbo;
  GLuint CommandBuffer;
  GLuint CounterBuffer;
  GLuint CoverageTex;
  CullProgram Cull;
  // Per mesh: visible when last tested (see lod_cull.comp).
  GLuint VisibilitySsbo;
  HizProgram Hiz;
  // The Hi-Z atlas and, per level, its first row, width and height; level 0
  // is unused (level k covers 2^k x 2^k pixels).
  GLuint HizTex;
  int HizTop;
  GLint HizLevels[HIZ_MAX_LEVELS][HIZ_LEVEL_FIELDS];

  // The section list last sent to the GPU.
  bool HasList;
  uint32_t ListRevision;
  int ListOpaque;
  int ListBorder;
  int64_t ListVertices;
  // Command slots: one per face run, opaque sections' first.
  int ListOpaqueRuns;
  int ListRuns;

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
  // The LOD pass's own colour (zero alpha where it drew nothing) and depth,
  // a texture so the Hi-Z pyramid can read it.
  GLuint LodColorTex;
  GLuint DepthLodTex;
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
  uint32_t *CullRecords;
  // Sections Meta and CullRecords have room for; grown on demand.
  int ListCapacity;

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

static CullProgram LoadCullProgram(void) {
  CullProgram Result = {0};
  Result.Program = GlLinkComputeProgram("assets/shaders/lod_cull.comp");
  Result.ViewProj = glGetUniformLocation(Result.Program, "uViewProj");
  Result.CameraBlock = glGetUniformLocation(Result.Program, "uCameraBlock");
  Result.CameraFrac = glGetUniformLocation(Result.Program, "uCameraFrac");
  Result.OpaqueCount = glGetUniformLocation(Result.Program, "uOpaqueCount");
  Result.OpaqueRuns = glGetUniformLocation(Result.Program, "uOpaqueRuns");
  Result.Total = glGetUniformLocation(Result.Program, "uTotal");
  Result.Compact = glGetUniformLocation(Result.Program, "uCompact");
  Result.CullEnabled = glGetUniformLocation(Result.Program, "uCullEnabled");
  Result.Phase = glGetUniformLocation(Result.Program, "uPhase");
  Result.PhaseBase = glGetUniformLocation(Result.Program, "uPhaseBase");
  Result.Viewport = glGetUniformLocation(Result.Program, "uViewport");
  Result.HizTop = glGetUniformLocation(Result.Program, "uHizTop");
  Result.HizLevels = glGetUniformLocation(Result.Program, "uHizLevels");
  return Result;
}

static HizProgram LoadHizProgram(void) {
  HizProgram Result = {0};
  Result.Program = GlLinkComputeProgram("assets/shaders/lod_hiz.comp");
  Result.Source = glGetUniformLocation(Result.Program, "uSource");
  Result.Top = glGetUniformLocation(Result.Program, "uTop");
  Result.Levels = glGetUniformLocation(Result.Program, "uHizLevels");
  glUseProgram(Result.Program);
  glUniform1i(glGetUniformLocation(Result.Program, "uDepth"), DEPTH_NEAR_UNIT);
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
  GLuint Textures[] = {State->DepthCopyTex, State->LodColorTex,
                       State->DepthLodTex, State->HizTex};
  for (size_t Idx = 0; Idx < sizeof(Textures) / sizeof(Textures[0]); Idx++) {
    if (Textures[Idx] != 0) {
      glDeleteTextures(1, &Textures[Idx]);
    }
  }
  State->FboCopy = 0;
  State->FboLod = 0;
  State->DepthLodTex = 0;
  State->HizTex = 0;
  State->HizTop = 0;
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

// Lays out levels 1.. of the Hi-Z pyramid for a Width x Height frame, one
// under the other in the atlas: level k is ceil(Width / 2^k) x
// ceil(Height / 2^k), down to the level where both are 1. Creates the atlas.
static void CreateHizAtlas(GlLodState *State, int Width, int Height) {
  memset(State->HizLevels, 0, sizeof(State->HizLevels));
  State->HizLevels[0][1] = Width;
  State->HizLevels[0][2] = Height;
  int Rows = 0;
  int Level = 1;
  for (; Level < HIZ_MAX_LEVELS; Level++) {
    int Size = 1 << Level;
    int LevelWidth = (Width + Size - 1) >> Level;
    int LevelHeight = (Height + Size - 1) >> Level;
    State->HizLevels[Level][0] = Rows;
    State->HizLevels[Level][1] = LevelWidth;
    State->HizLevels[Level][2] = LevelHeight;
    State->HizLevels[Level][3] = 0;
    Rows += LevelHeight;
    if (LevelWidth == 1 && LevelHeight == 1) {
      break;
    }
  }
  State->HizTop = (Level < HIZ_MAX_LEVELS) ? Level : HIZ_MAX_LEVELS - 1;
  State->HizTex = CreateTargetTexture(GL_R32F, GL_RED, GL_FLOAT,
                                      State->HizLevels[1][1], Rows);
}

// The depth copy matches the default framebuffer's 24/8 depth-stencil, which
// a depth blit requires; the LOD depth is 32-bit float, read only by Hi-Z.
static bool CreateTargets(GlLodState *State, int Width, int Height) {
  DeleteTargets(State);
  State->DepthCopyTex = CreateTargetTexture(
      GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, Width, Height);
  State->LodColorTex =
      CreateTargetTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, Width, Height);
  State->DepthLodTex = CreateTargetTexture(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT,
                                           GL_FLOAT, Width, Height);
  if (State->HizEnabled) {
    CreateHizAtlas(State, Width, Height);
  }

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
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                         State->DepthLodTex, 0);
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
  glGenBuffers(1, &State->CullSsbo);
  glGenBuffers(1, &State->CommandBuffer);
  glGenBuffers(1, &State->CounterBuffer);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, State->CounterBuffer);
  glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * COUNTER_UINTS),
               NULL, GL_DYNAMIC_DRAW);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  glGenVertexArrays(1, &State->RemapVao);
  if (State->HizEnabled) {
    // Every mesh starts visible, so a section is never hidden before a test.
    uint32_t *Ones = (uint32_t *)malloc(sizeof(uint32_t) * LOD_MAX_MESHES);
    if (Ones == NULL) {
      return false;
    }
    for (int Idx = 0; Idx < LOD_MAX_MESHES; Idx++) {
      Ones[Idx] = 1U;
    }
    glGenBuffers(1, &State->VisibilitySsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, State->VisibilitySsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr)(sizeof(uint32_t) * LOD_MAX_MESHES), Ones, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    free(Ones);
  }
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

// Grows the section record buffers to hold Count sections; false (and the
// buffers unchanged) when memory runs out.
static bool EnsureListCapacity(GlLodState *State, int Count) {
  if (Count <= State->ListCapacity) {
    return true;
  }
  int Capacity = (State->ListCapacity > 0) ? State->ListCapacity : LIST_INITIAL_CAPACITY;
  while (Capacity < Count) {
    Capacity *= 2;
  }
  int32_t *Meta = (int32_t *)realloc(
      State->Meta, sizeof(int32_t) * LOD_META_COMPONENTS * (size_t)Capacity);
  if (Meta == NULL) {
    return false;
  }
  State->Meta = Meta;
  uint32_t *Records = (uint32_t *)realloc(
      State->CullRecords, sizeof(uint32_t) * CULL_RECORD_UINTS * (size_t)Capacity);
  if (Records == NULL) {
    return false;
  }
  State->CullRecords = Records;
  State->ListCapacity = Capacity;
  return true;
}

bool GlLodInit(GLuint SharedIndexBuffer, const GlLodOptions *Options) {
  GlLodState *State = GetLodState();
  *State = (GlLodState){0};
  State->SharedIndexBuffer = SharedIndexBuffer;
  State->UseIndirectCount = Options->UseIndirectCount;
  State->CullEnabled = Options->Cull;
  State->HizEnabled = Options->Cull && Options->HiZ;
  State->PageUsed = (uint8_t *)calloc(LOD_ARENA_PAGES, sizeof(uint8_t));
  State->Meshes = (LodMeshRecord *)calloc(LOD_MAX_MESHES, sizeof(LodMeshRecord));
  State->FreeMeshes = (int *)malloc(sizeof(int) * LOD_MAX_MESHES);
  if (State->PageUsed == NULL || State->Meshes == NULL ||
      State->FreeMeshes == NULL || !EnsureListCapacity(State, LIST_INITIAL_CAPACITY) ||
      !CreateArena(State) || !CreateCoverage(State)) {
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
  State->Cull = LoadCullProgram();
  if (State->HizEnabled) {
    State->Hiz = LoadHizProgram();
  }

  State->Stats.ArenaCapacityBytes =
      (int64_t)LOD_ARENA_PAGES * LOD_PAGE_VERTICES * (int64_t)sizeof(ChunkVertex);
  State->Active = true;
  LogInfo("RENDER: LOD pipeline on: %d-page arena (%lld MB), projection %.0f..%.0f, "
          "standard depth (32-bit float LOD depth), 1:1 left in the default "
          "framebuffer, GPU cull%s into a %s command list, Hi-Z occlusion %s",
          LOD_ARENA_PAGES,
          (long long)(State->Stats.ArenaCapacityBytes >> BYTES_TO_MB_SHIFT), LOD_NEAR_PLANE,
          LOD_FAR_PLANE, Options->Cull ? "" : " (off: --lod-cull off)",
          Options->UseIndirectCount ? "compacted (indirect count)" : "fixed (zeroed)",
          State->HizEnabled ? "on (two phases)" : "off");
  State->Stats.IndirectCount = Options->UseIndirectCount;
  State->Stats.HiZ = State->HizEnabled;
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
  DeleteBuffer(&State->CullSsbo);
  DeleteBuffer(&State->CommandBuffer);
  DeleteBuffer(&State->CounterBuffer);
  DeleteBuffer(&State->VisibilitySsbo);
  if (State->Vao != 0) {
    glDeleteVertexArrays(1, &State->Vao);
  }
  if (State->RemapVao != 0) {
    glDeleteVertexArrays(1, &State->RemapVao);
  }
  if (State->CoverageTex != 0) {
    glDeleteTextures(1, &State->CoverageTex);
  }
  GLuint Programs[] = {State->RemapProgram, State->CompositeProgram,
                       State->Opaque.Program, State->Border.Program,
                       State->Cull.Program, State->Hiz.Program};
  for (size_t Idx = 0; Idx < sizeof(Programs) / sizeof(Programs[0]); Idx++) {
    if (Programs[Idx] != 0) {
      glDeleteProgram(Programs[Idx]);
    }
  }
  free(State->PageUsed);
  free(State->Meshes);
  free(State->FreeMeshes);
  free(State->Pending);
  free(State->Meta);
  free(State->CullRecords);
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

RenderLodMesh GlLodUploadMesh(const ChunkVertex *Vertices,
                              const RenderLodRun *Runs, int RunCount) {
  GlLodState *State = GetLodState();
  int VertexCount = 0;
  for (int Run = 0; Run < RunCount; Run++) {
    VertexCount += Runs[Run].Quads * LOD_VERTICES_PER_QUAD;
  }
  if (!State->Active || VertexCount <= 0 || RunCount > LOD_RUNS ||
      State->FreeMeshCount == 0) {
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
  LodMeshRecord *Record = &State->Meshes[Mesh];
  Record->RunCount = RunCount;
  for (int Run = 0; Run < RunCount; Run++) {
    Record->Runs[Run] = ((uint32_t)Runs[Run].Group << CULL_RUN_GROUP_SHIFT) |
                        (uint32_t)Runs[Run].Quads;
  }
  State->LiveMeshes++;
  if (State->HizEnabled) {
    uint32_t Visible = 1U;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, State->VisibilitySsbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)((size_t)Mesh * sizeof(uint32_t)),
                    (GLsizeiptr)sizeof(Visible), &Visible);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }
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

// Fills the section records the vertex shader and the cull read; returns the
// vertices of every section, before culling.
static int64_t BuildSectionRecords(GlLodState *State, const RenderLodDraw *Draws,
                                   int Count) {
  int64_t Vertices = 0;
  int Slots = 0;
  for (int Idx = 0; Idx < Count; Idx++) {
    if (Idx == State->ListOpaque) {
      State->ListOpaqueRuns = Slots;
    }
    const RenderLodDraw *Draw = &Draws[Idx];
    const LodMeshRecord *Record = &State->Meshes[Draw->Mesh];
    int32_t *Meta = &State->Meta[(size_t)Idx * LOD_META_COMPONENTS];
    Meta[0] = Draw->OriginX;
    Meta[1] = Draw->OriginY;
    Meta[2] = Draw->OriginZ;
    Meta[3] = Draw->Scale;
    uint32_t *Cull = &State->CullRecords[(size_t)Idx * CULL_RECORD_UINTS];
    Cull[0] = (uint32_t)(Record->FirstPage * LOD_PAGE_VERTICES);
    Cull[1] = (uint32_t)Record->RunCount;
    Cull[2] = (uint32_t)Slots;
    Cull[3] = (uint32_t)Draw->Mesh;
    memcpy(&Cull[CULL_RECORD_HEADER], Record->Runs,
           sizeof(uint32_t) * (size_t)Record->RunCount);
    Slots += Record->RunCount;
    Vertices += Record->VertexCount;
  }
  if (Count == State->ListOpaque) {
    State->ListOpaqueRuns = Slots;
  }
  State->ListRuns = Slots;
  return Vertices;
}

static void UploadSectionList(GlLodState *State, const RenderLodDraw *Draws,
                              int OpaqueCount, int BorderCount,
                              uint32_t ListRevision) {
  int Total = OpaqueCount + BorderCount;
  if (!EnsureListCapacity(State, Total)) {
    State->HasList = false;
    return;
  }
  State->ListOpaque = OpaqueCount;
  State->ListBorder = BorderCount;
  State->ListVertices = BuildSectionRecords(State, Draws, Total);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, State->MetaSsbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER,
               (GLsizeiptr)(sizeof(int32_t) * LOD_META_COMPONENTS * (size_t)Total),
               State->Meta, GL_STATIC_DRAW);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, State->CullSsbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER,
               (GLsizeiptr)(sizeof(uint32_t) * CULL_RECORD_UINTS * (size_t)Total),
               State->CullRecords, GL_STATIC_DRAW);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  glBindBuffer(GL_DRAW_INDIRECT_BUFFER, State->CommandBuffer);
  glBufferData(GL_DRAW_INDIRECT_BUFFER,
               (GLsizeiptr)(sizeof(LodIndirectCommand) * (size_t)State->ListRuns *
                            (State->HizEnabled ? 2U : 1U)),
               NULL, GL_DYNAMIC_DRAW);
  glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
  State->HasList = true;
  State->ListRevision = ListRevision;
}

void GlLodReadCullStats(void) {
  GlLodState *State = GetLodState();
  if (!State->Active || !State->HasList) {
    return;
  }
  uint32_t Counters[COUNTER_UINTS];
  glBindBuffer(GL_COPY_READ_BUFFER, State->CounterBuffer);
  glGetBufferSubData(GL_COPY_READ_BUFFER, 0, (GLsizeiptr)sizeof(Counters), Counters);
  glBindBuffer(GL_COPY_READ_BUFFER, 0);
  RenderLodStats *Stats = &State->Stats;
  Stats->CullValid = true;
  Stats->FrustumCulledSections = (int)Counters[COUNTER_FRUSTUM_CULLED];
  Stats->CandidateRuns = (int)Counters[COUNTER_CANDIDATE_RUNS];
  Stats->FacingCulledRuns = (int)Counters[COUNTER_FACING_CULLED];
  Stats->HizCulledSections = (int)Counters[COUNTER_HIZ_CULLED];
  Stats->Phase1Commands = (int)Counters[COUNTER_PHASE1_COMMANDS];
  Stats->Phase2Commands = (int)Counters[COUNTER_PHASE2_COMMANDS];
  Stats->Phase1Vertices = (int64_t)Counters[COUNTER_PHASE1_QUADS] * LOD_VERTICES_PER_QUAD;
  Stats->Phase2Vertices = (int64_t)Counters[COUNTER_PHASE2_QUADS] * LOD_VERTICES_PER_QUAD;
  Stats->DrawnCommands = Stats->Phase1Commands + Stats->Phase2Commands;
  Stats->DrawnVertices = Stats->Phase1Vertices + Stats->Phase2Vertices;
}

static void ClearCounters(const GlLodState *State) {
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, State->CounterBuffer);
  glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER,
                    GL_UNSIGNED_INT, NULL);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// The frame's view for the cull and the draws.
typedef struct {
  float ViewProj[MAT4_FLOATS];
  int CameraBlock[3];
  float CameraFrac[3];
} LodView;

// Runs one phase of lod_cull.comp over every section, writing that phase's
// indirect commands and the counters, and makes them visible to the draws and
// the phase that follow.
static void CullSections(GlLodState *State, const LodView *View, int Phase,
                         const GlLodFrame *Frame) {
  int Total = State->ListOpaque + State->ListBorder;
  const float *ViewProj = View->ViewProj;
  const int *CameraBlock = View->CameraBlock;
  const float *CameraFrac = View->CameraFrac;
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING_META, State->MetaSsbo);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING_CULL, State->CullSsbo);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING_COMMANDS, State->CommandBuffer);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING_COUNTERS, State->CounterBuffer);
  const CullProgram *Cull = &State->Cull;
  glUseProgram(Cull->Program);
  glUniformMatrix4fv(Cull->ViewProj, 1, GL_FALSE, ViewProj);
  glUniform3i(Cull->CameraBlock, CameraBlock[0], CameraBlock[1], CameraBlock[2]);
  glUniform3f(Cull->CameraFrac, CameraFrac[0], CameraFrac[1], CameraFrac[2]);
  glUniform1ui(Cull->OpaqueCount, (GLuint)State->ListOpaque);
  glUniform1ui(Cull->OpaqueRuns, (GLuint)State->ListOpaqueRuns);
  glUniform1ui(Cull->Total, (GLuint)Total);
  glUniform1i(Cull->Compact, State->UseIndirectCount ? 1 : 0);
  glUniform1i(Cull->CullEnabled, State->CullEnabled ? 1 : 0);
  glUniform1i(Cull->Phase, Phase);
  glUniform1ui(Cull->PhaseBase,
               (Phase == PHASE_SECOND) ? (GLuint)State->ListRuns : 0U);
  if (State->HizEnabled) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING_VISIBILITY, State->VisibilitySsbo);
    glUniform2f(Cull->Viewport, (float)Frame->Width, (float)Frame->Height);
    glUniform1i(Cull->HizTop, State->HizTop);
    glUniform4iv(Cull->HizLevels, HIZ_MAX_LEVELS, &State->HizLevels[0][0]);
    glBindImageTexture(HIZ_IMAGE_UNIT, State->HizTex, 0, GL_FALSE, 0, GL_READ_ONLY,
                       GL_R32F);
  }
  glDispatchCompute((GLuint)((Total + CULL_WORKGROUP - 1) / CULL_WORKGROUP), 1, 1);
  GLbitfield Barriers = GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                        GL_BUFFER_UPDATE_BARRIER_BIT;
  if (State->UseIndirectCount) {
    Barriers |= LOD_PARAMETER_BUFFER_BARRIER_BIT;
  }
  glMemoryBarrier(Barriers);
  for (int Binding = BINDING_CULL; Binding <= BINDING_VISIBILITY; Binding++) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)Binding, 0);
  }
  if (State->HizEnabled) {
    glBindImageTexture(HIZ_IMAGE_UNIT, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
  }
}

// Max-reduces the LOD depth into the Hi-Z atlas, five levels per dispatch.
// The LOD framebuffer is unbound first, so its depth is only read, never at
// once a render target.
static void BuildHizPyramid(const GlLodState *State) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glUseProgram(State->Hiz.Program);
  glUniform1i(State->Hiz.Top, State->HizTop);
  glUniform4iv(State->Hiz.Levels, HIZ_MAX_LEVELS, &State->HizLevels[0][0]);
  glActiveTexture(GL_TEXTURE0 + DEPTH_NEAR_UNIT);
  glBindTexture(GL_TEXTURE_2D, State->DepthLodTex);
  glBindImageTexture(HIZ_IMAGE_UNIT, State->HizTex, 0, GL_FALSE, 0, GL_READ_WRITE,
                     GL_R32F);
  for (int Source = 0; Source < State->HizTop; Source += HIZ_LEVELS_PER_DISPATCH) {
    const GLint *First = State->HizLevels[Source + 1];
    glUniform1i(State->Hiz.Source, Source);
    glDispatchCompute((GLuint)((First[1] + HIZ_WORKGROUP - 1) / HIZ_WORKGROUP),
                      (GLuint)((First[2] + HIZ_WORKGROUP - 1) / HIZ_WORKGROUP), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
  }
  glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
  glBindImageTexture(HIZ_IMAGE_UNIT, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0 + BLOCK_UNIT);
}

// One pass's commands: Count slots from slot First on.
static void DrawPass(const GlLodState *State, int First, int Count,
                     int CounterIndex) {
  size_t Offset = (size_t)First * sizeof(LodIndirectCommand);
  GLsizei MaxDraws = (GLsizei)Count;
  if (State->UseIndirectCount) {
    GlCapsProcs()->MultiDrawElementsIndirectCount(
        GL_TRIANGLES, GL_UNSIGNED_SHORT, BufferOffset(Offset),
        (GLintptr)((size_t)CounterIndex * sizeof(uint32_t)), MaxDraws,
        (GLsizei)sizeof(LodIndirectCommand));
  } else {
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
                                BufferOffset(Offset), MaxDraws,
                                (GLsizei)sizeof(LodIndirectCommand));
  }
}

// One phase's opaque and border multi-draws into the LOD target: its commands
// start at slot PhaseBase and its draw counts at counter CounterBase.
static void DrawPhase(const GlLodState *State, const GlLodFrame *Frame,
                      const LodView *View, int PhaseBase, int CounterBase) {
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING_META, State->MetaSsbo);
  glBindBuffer(GL_DRAW_INDIRECT_BUFFER, State->CommandBuffer);
  if (State->UseIndirectCount) {
    glBindBuffer(LOD_PARAMETER_BUFFER, State->CounterBuffer);
  }
  glBindVertexArray(State->Vao);
  glActiveTexture(GL_TEXTURE0 + BLOCK_UNIT);
  glBindTexture(GL_TEXTURE_2D_ARRAY, Frame->BlockArrayTex);
  glBindSampler(BLOCK_UNIT, Frame->LodSampler);
  glActiveTexture(GL_TEXTURE0 + COVERAGE_UNIT);
  glBindTexture(GL_TEXTURE_2D, State->CoverageTex);

  if (State->ListOpaque > 0) {
    UseLodProgram(&State->Opaque, View->ViewProj, View->CameraBlock, View->CameraFrac);
    DrawPass(State, PhaseBase, State->ListOpaqueRuns, CounterBase);
  }
  if (State->ListBorder > 0) {
    UseLodProgram(&State->Border, View->ViewProj, View->CameraBlock, View->CameraFrac);
    DrawPass(State, PhaseBase + State->ListOpaqueRuns,
             State->ListRuns - State->ListOpaqueRuns, CounterBase + 1);
  }
  if (State->UseIndirectCount) {
    glBindBuffer(LOD_PARAMETER_BUFFER, 0);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0 + BLOCK_UNIT);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  glBindSampler(BLOCK_UNIT, 0);
  glBindVertexArray(0);
  glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING_META, 0);
}

void GlLodDraw(const GlLodFrame *Frame, const RenderLodDraw *Draws,
               int OpaqueCount, int BorderCount, uint32_t ListRevision) {
  GlLodState *State = GetLodState();
  State->Stats.OpaqueDraws = 0;
  State->Stats.BorderDraws = 0;
  State->Stats.CandidateVertices = 0;
  int Total = OpaqueCount + BorderCount;
  if (!State->Active) {
    return;
  }
  if (Total <= 0) {
    State->Stats.DrawnVertices = 0;
    State->Stats.DrawnCommands = 0;
    return;
  }
  if ((State->Width != Frame->Width || State->Height != Frame->Height) &&
      !CreateTargets(State, Frame->Width, Frame->Height)) {
    return;
  }
  // The section index reaches the shader through the identity instance
  // buffer, which holds LOD_MAX_DRAWS entries (lod_renderer never selects more).
  if (Total > LOD_MAX_DRAWS) {
    return;
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
  GlMat4 LookAt = GlMat4LookAt((Vec3){0.0F, 0.0F, 0.0F}, Forward, Frame->Camera.Up);
  LodView View;
  GlMat4ToColumnMajor(GlMat4Multiply(LookAt, Projection), View.ViewProj);
  memcpy(View.CameraBlock, CameraBlock, sizeof(CameraBlock));
  memcpy(View.CameraFrac, CameraFrac, sizeof(CameraFrac));

  // The LOD is always drawn filled. Under the wireframe debug view its edges
  // would land exactly on chunk-column borders, where the border shader's
  // coverage lookup cannot tell which column a line belongs to.
  if (!State->HasList || State->ListRevision != ListRevision ||
      State->ListOpaque != OpaqueCount || State->ListBorder != BorderCount) {
    UploadSectionList(State, Draws, OpaqueCount, BorderCount, ListRevision);
    if (!State->HasList) {
      return;
    }
  }
  if (Frame->Wireframe) {
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  }
  RemapDepth(State, Projection.m10, Projection.m14, Frame);
  State->Stats.CandidateVertices = State->ListVertices;
  ClearCounters(State);
  CullSections(State, &View, State->HizEnabled ? PHASE_FIRST : PHASE_SINGLE, Frame);
  DrawPhase(State, Frame, &View, 0, COUNTER_OPAQUE);
  if (State->HizEnabled) {
    BuildHizPyramid(State);
    CullSections(State, &View, PHASE_SECOND, Frame);
    glBindFramebuffer(GL_FRAMEBUFFER, State->FboLod);
    DrawPhase(State, Frame, &View, State->ListRuns, COUNTER_PHASE2_OPAQUE);
  }
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
