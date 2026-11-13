#include "render/gl_mesh_arena.h"

#include "core/log.h"
#include "world/world_limits.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Each page-origin entry is one RGBA32F texel: the chunk origin in xyz. Float,
// not integer, because the origin sits half a block off the chunk grid, and a
// float texel reproduces exactly the value the per-chunk uniform used to hold.
enum {
  ORIGIN_COMPONENTS = 4,
  PAGE_BYTES = ARENA_PAGE_VERTICES * (int)sizeof(ChunkVertex),
  ORIGIN_TEXEL_BYTES = ORIGIN_COMPONENTS * (int)sizeof(float),
  // 16 MiB of vertices per block: large enough that a pass needs few draws,
  // small enough to allocate on demand on an integrated GPU.
  MAX_PAGES_PER_BLOCK = 8192,
  // A mesh never spans more pages than this (CHUNK_MAX_VERTICES / page size).
  MAX_PAGES_PER_MESH = (CHUNK_MAX_VERTICES + ARENA_PAGE_VERTICES - 1) / ARENA_PAGE_VERTICES,
  // Free runs alternate with used ones, so a block never holds more than this.
  MAX_FREE_RUNS = (MAX_PAGES_PER_BLOCK / 2) + 1,
  // Every live mesh can be released within the delay window at most once
  // more than the pool holds; beyond that the ring drains synchronously.
  PENDING_CAPACITY = MAX_ACTIVE_CHUNKS * 3
};

typedef struct {
  int First;
  int Count;
} PageSpan;

typedef struct {
  GLuint VertexArray;
  GLuint VertexBuffer;
  GLuint OriginBuffer;
  GLuint OriginTexture;
  PageSpan FreeSpans[MAX_FREE_RUNS];
  int FreeSpanCount;
  int UsedPages;
} ArenaBlock;

typedef struct {
  ArenaRun Run;
  uint32_t ReleaseFrame;
} PendingRelease;

typedef struct {
  GLuint SharedIndexBuffer;
  int PagesPerBlock;
  ArenaBlock Blocks[ARENA_MAX_BLOCKS];
  int BlockCount;

  PendingRelease Pending[PENDING_CAPACITY];
  int PendingHead;
  int PendingCount;
  int64_t PendingPages;
  uint32_t Frame;

  int64_t LiveVertexBytes;
  int64_t LivePageBytes;

  float OriginScratch[MAX_PAGES_PER_MESH * ORIGIN_COMPONENTS];
} ArenaState;

static ArenaState *GetArenaState(void) {
  static ArenaState State;
  return &State;
}

static const void *BufferOffset(size_t Offset) {
  union {
    uintptr_t Offset;
    const void *Pointer;
  } Converted = {.Offset = (uintptr_t)Offset};
  return Converted.Pointer;
}

bool ArenaInit(GLuint SharedIndexBuffer) {
  ArenaState *State = GetArenaState();
  memset(State, 0, sizeof(*State));
  State->SharedIndexBuffer = SharedIndexBuffer;

  GLint MaxTexels = 0;
  glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &MaxTexels);
  State->PagesPerBlock = MAX_PAGES_PER_BLOCK;
  if (MaxTexels > 0 && MaxTexels < State->PagesPerBlock) {
    State->PagesPerBlock = (int)MaxTexels;
  }
  if (State->PagesPerBlock < MAX_PAGES_PER_MESH) {
    LogError("RENDER: texture buffers hold only %d texels; a chunk mesh needs %d",
             (int)MaxTexels, MAX_PAGES_PER_MESH);
    return false;
  }
  return true;
}

void ArenaShutdown(void) {
  ArenaState *State = GetArenaState();
  for (int Idx = 0; Idx < State->BlockCount; Idx++) {
    ArenaBlock *Block = &State->Blocks[Idx];
    glDeleteTextures(1, &Block->OriginTexture);
    glDeleteBuffers(1, &Block->OriginBuffer);
    glDeleteBuffers(1, &Block->VertexBuffer);
    glDeleteVertexArrays(1, &Block->VertexArray);
  }
  State->BlockCount = 0;
  State->PendingCount = 0;
}

static ArenaBlock *CreateBlock(void) {
  ArenaState *State = GetArenaState();
  if (State->BlockCount >= ARENA_MAX_BLOCKS) {
    return NULL;
  }
  ArenaBlock *Block = &State->Blocks[State->BlockCount];
  memset(Block, 0, sizeof(*Block));

  glGenVertexArrays(1, &Block->VertexArray);
  glGenBuffers(1, &Block->VertexBuffer);
  glBindVertexArray(Block->VertexArray);
  glBindBuffer(GL_ARRAY_BUFFER, Block->VertexBuffer);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)State->PagesPerBlock * PAGE_BYTES,
               NULL, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribIPointer(0, 2, GL_UNSIGNED_INT, (GLsizei)sizeof(ChunkVertex),
                         BufferOffset(0));
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, State->SharedIndexBuffer);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  glGenBuffers(1, &Block->OriginBuffer);
  glBindBuffer(GL_TEXTURE_BUFFER, Block->OriginBuffer);
  glBufferData(GL_TEXTURE_BUFFER,
               (GLsizeiptr)State->PagesPerBlock * ORIGIN_TEXEL_BYTES, NULL,
               GL_DYNAMIC_DRAW);
  glGenTextures(1, &Block->OriginTexture);
  glBindTexture(GL_TEXTURE_BUFFER, Block->OriginTexture);
  glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, Block->OriginBuffer);
  glBindTexture(GL_TEXTURE_BUFFER, 0);
  glBindBuffer(GL_TEXTURE_BUFFER, 0);

  Block->FreeSpans[0] = (PageSpan){.First = 0, .Count = State->PagesPerBlock};
  Block->FreeSpanCount = 1;
  State->BlockCount++;
  return Block;
}

// First fit within one block. -1 when no span is long enough.
static int TakePages(ArenaBlock *Block, int Pages) {
  for (int Idx = 0; Idx < Block->FreeSpanCount; Idx++) {
    PageSpan *Span = &Block->FreeSpans[Idx];
    if (Span->Count < Pages) {
      continue;
    }
    int First = Span->First;
    Span->First += Pages;
    Span->Count -= Pages;
    if (Span->Count == 0) {
      memmove(&Block->FreeSpans[Idx], &Block->FreeSpans[Idx + 1],
              (size_t)(Block->FreeSpanCount - Idx - 1) * sizeof(PageSpan));
      Block->FreeSpanCount--;
    }
    Block->UsedPages += Pages;
    return First;
  }
  return -1;
}

// Inserts a span into the offset-sorted free list, merging it with the spans
// it touches so adjacent releases become one allocatable run.
static void ReturnPages(ArenaBlock *Block, int First, int Count) {
  int Pos = 0;
  while (Pos < Block->FreeSpanCount && Block->FreeSpans[Pos].First < First) {
    Pos++;
  }
  bool MergePrev = Pos > 0 && (Block->FreeSpans[Pos - 1].First +
                               Block->FreeSpans[Pos - 1].Count) == First;
  bool MergeNext = Pos < Block->FreeSpanCount &&
                   (First + Count) == Block->FreeSpans[Pos].First;

  if (MergePrev && MergeNext) {
    Block->FreeSpans[Pos - 1].Count += Count + Block->FreeSpans[Pos].Count;
    memmove(&Block->FreeSpans[Pos], &Block->FreeSpans[Pos + 1],
            (size_t)(Block->FreeSpanCount - Pos - 1) * sizeof(PageSpan));
    Block->FreeSpanCount--;
  } else if (MergePrev) {
    Block->FreeSpans[Pos - 1].Count += Count;
  } else if (MergeNext) {
    Block->FreeSpans[Pos].First = First;
    Block->FreeSpans[Pos].Count += Count;
  } else if (Block->FreeSpanCount < MAX_FREE_RUNS) {
    memmove(&Block->FreeSpans[Pos + 1], &Block->FreeSpans[Pos],
            (size_t)(Block->FreeSpanCount - Pos) * sizeof(PageSpan));
    Block->FreeSpans[Pos] = (PageSpan){.First = First, .Count = Count};
    Block->FreeSpanCount++;
  }
  Block->UsedPages -= Count;
}

bool ArenaUpload(const ChunkVertex *Vertices, int VertexCount, Vec3 Origin,
                 ArenaRun *Out) {
  ArenaState *State = GetArenaState();
  if (Vertices == NULL || VertexCount <= 0 || VertexCount > CHUNK_MAX_VERTICES) {
    return false;
  }
  int Pages = (VertexCount + ARENA_PAGE_VERTICES - 1) / ARENA_PAGE_VERTICES;

  int BlockIdx = -1;
  int First = -1;
  for (int Idx = 0; Idx < State->BlockCount && First < 0; Idx++) {
    First = TakePages(&State->Blocks[Idx], Pages);
    BlockIdx = Idx;
  }
  if (First < 0) {
    ArenaBlock *Fresh = CreateBlock();
    if (Fresh == NULL) {
      LogWarn("RENDER: chunk mesh arena full (%d blocks)", ARENA_MAX_BLOCKS);
      return false;
    }
    BlockIdx = State->BlockCount - 1;
    First = TakePages(Fresh, Pages);
  }
  const ArenaBlock *Block = &State->Blocks[BlockIdx];

  glBindBuffer(GL_ARRAY_BUFFER, Block->VertexBuffer);
  glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)First * PAGE_BYTES,
                  (GLsizeiptr)((size_t)VertexCount * sizeof(ChunkVertex)),
                  Vertices);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  for (int Page = 0; Page < Pages; Page++) {
    float *Texel = &State->OriginScratch[(size_t)Page * ORIGIN_COMPONENTS];
    Texel[0] = Origin.x;
    Texel[1] = Origin.y;
    Texel[2] = Origin.z;
    Texel[3] = 0.0F;
  }
  glBindBuffer(GL_TEXTURE_BUFFER, Block->OriginBuffer);
  glBufferSubData(GL_TEXTURE_BUFFER, (GLintptr)First * ORIGIN_TEXEL_BYTES,
                  (GLsizeiptr)Pages * ORIGIN_TEXEL_BYTES, State->OriginScratch);
  glBindBuffer(GL_TEXTURE_BUFFER, 0);

  State->LiveVertexBytes += (int64_t)VertexCount * (int64_t)sizeof(ChunkVertex);
  State->LivePageBytes += (int64_t)Pages * PAGE_BYTES;
  *Out = (ArenaRun){.Block = BlockIdx, .FirstPage = First, .PageCount = Pages};
  return true;
}

static void RecyclePending(int Count) {
  ArenaState *State = GetArenaState();
  for (int Idx = 0; Idx < Count && State->PendingCount > 0; Idx++) {
    const PendingRelease *Entry = &State->Pending[State->PendingHead];
    ReturnPages(&State->Blocks[Entry->Run.Block], Entry->Run.FirstPage,
                Entry->Run.PageCount);
    State->PendingPages -= Entry->Run.PageCount;
    State->PendingHead = (State->PendingHead + 1) % PENDING_CAPACITY;
    State->PendingCount--;
  }
}

void ArenaRelease(ArenaRun Run, int VertexCount) {
  ArenaState *State = GetArenaState();
  if (Run.Block < 0 || Run.Block >= State->BlockCount || Run.PageCount <= 0) {
    return;
  }
  State->LiveVertexBytes -= (int64_t)VertexCount * (int64_t)sizeof(ChunkVertex);
  State->LivePageBytes -= (int64_t)Run.PageCount * PAGE_BYTES;

  // Only reachable when far more meshes were freed within the delay window
  // than the pool holds. Waiting for the GPU makes every pending run safe.
  if (State->PendingCount == PENDING_CAPACITY) {
    glFinish();
    RecyclePending(State->PendingCount);
  }
  int Tail = (State->PendingHead + State->PendingCount) % PENDING_CAPACITY;
  State->Pending[Tail] = (PendingRelease){
      .Run = Run, .ReleaseFrame = State->Frame + ARENA_RELEASE_DELAY_FRAMES};
  State->PendingCount++;
  State->PendingPages += Run.PageCount;
}

void ArenaEndFrame(void) {
  ArenaState *State = GetArenaState();
  State->Frame++;
  // Entries were queued in frame order, so the ready ones are at the head.
  int Ready = 0;
  while (Ready < State->PendingCount) {
    int Slot = (State->PendingHead + Ready) % PENDING_CAPACITY;
    if ((int32_t)(State->Frame - State->Pending[Slot].ReleaseFrame) < 0) {
      break;
    }
    Ready++;
  }
  RecyclePending(Ready);
}

int ArenaBlockCount(void) { return GetArenaState()->BlockCount; }

GLuint ArenaBlockVertexArray(int Block) {
  return GetArenaState()->Blocks[Block].VertexArray;
}

GLuint ArenaBlockOriginTexture(int Block) {
  return GetArenaState()->Blocks[Block].OriginTexture;
}

void ArenaGetStats(ArenaStats *Out) {
  const ArenaState *State = GetArenaState();
  int64_t Used = 0;
  for (int Idx = 0; Idx < State->BlockCount; Idx++) {
    Used += State->Blocks[Idx].UsedPages;
  }
  int64_t Total = (int64_t)State->BlockCount * State->PagesPerBlock;
  Out->Blocks = State->BlockCount;
  Out->UsedPages = Used - State->PendingPages;
  Out->PendingPages = State->PendingPages;
  Out->FreePages = Total - Used;
  Out->UsedBytes = State->LivePageBytes;
  Out->SlackBytes = State->LivePageBytes - State->LiveVertexBytes;
}
