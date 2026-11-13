#ifndef RENDER_GL_MESH_ARENA_H
#define RENDER_GL_MESH_ARENA_H

#include "render/backend.h"
#include "render/gl_api.h"
#include "core/vecmath.h"
#include <stdbool.h>
#include <stdint.h>

// GPU memory for chunk meshes, private to the GL backend.
//
// Meshes are suballocated from large blocks instead of owning a buffer each, so
// that every mesh in a block shares one vertex array and a whole pass can be
// drawn with one multi-draw per block. A block is cut into pages of
// ARENA_PAGE_VERTICES vertices; a mesh takes a contiguous run of whole pages,
// and each page records the world origin of the chunk owning it in the block's
// page-origin texture buffer. The chunk vertex shader finds that origin from
// gl_VertexID alone, which is what lets one draw cover many chunks.

// The page size exponent. The vertex shader receives the same value at compile
// time, so the two cannot disagree.
#define ARENA_PAGE_SHIFT 8

enum {
  ARENA_PAGE_VERTICES = 1 << ARENA_PAGE_SHIFT,
  // A freed run stays reserved this many presented frames, so no upload can
  // overwrite vertices a frame still queued on the GPU reads.
  ARENA_RELEASE_DELAY_FRAMES = 3,
  ARENA_MAX_BLOCKS = 64
};

// Where a mesh lives: a run of whole pages in one block.
typedef struct {
  int Block;
  int FirstPage;
  int PageCount;
} ArenaRun;

typedef struct {
  int Blocks;
  int64_t UsedPages;
  int64_t FreePages;
  int64_t PendingPages;
  // Bytes of the pages live meshes occupy, and the part of them that is page
  // rounding rather than vertex data.
  int64_t UsedBytes;
  int64_t SlackBytes;
} ArenaStats;

// SharedIndexBuffer is the static quad index buffer every block's vertex array
// binds. Returns false when the driver cannot index even one full mesh's pages
// through a texture buffer.
bool ArenaInit(GLuint SharedIndexBuffer);
void ArenaShutdown(void);

// Copies the vertices into a free run and stamps Origin on every page of it.
// False (and Out untouched) when no block has room and no block can be added.
bool ArenaUpload(const ChunkVertex *Vertices, int VertexCount, Vec3 Origin,
                 ArenaRun *Out);

// Gives a run back once ARENA_RELEASE_DELAY_FRAMES more frames have been
// presented. VertexCount is the mesh's, for the slack figure.
void ArenaRelease(ArenaRun Run, int VertexCount);

// Called once per presented frame; recycles runs whose delay has passed.
void ArenaEndFrame(void);

int ArenaBlockCount(void);
GLuint ArenaBlockVertexArray(int Block);
GLuint ArenaBlockOriginTexture(int Block);
void ArenaGetStats(ArenaStats *Out);

#endif
