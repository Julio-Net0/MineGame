#ifndef CHUNK_H
#define CHUNK_H

#include "core/vecmath.h"
#include "core/mesh_handle.h"
#include "world/biome.h"
#include "world/block_id.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define CHUNK_SIZE 16
#define CHUNK_VOLUME ((__SIZE_TYPE__)CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)
#define CHUNK_HALF_SIZE (CHUNK_SIZE / 2.0F)

#define BLOCK_SIZE 1.0F
#define BLOCK_HALF_SIZE (BLOCK_SIZE / 2.0F)

// Biomes are stored one id per BIOME_CELL_SIZE^3 block cell, not per block: 64
// bytes a chunk instead of 4096. Climate is a low-frequency field, so a cell's
// worth of blocks share a biome anyway, and the coarser grid also cuts the
// sampling generation has to do by the same factor.
#define BIOME_CELLS_PER_CHUNK (CHUNK_SIZE / BIOME_CELL_SIZE)

// Highest Y filled with water during generation. Shared so the feature pass can
// keep trees off submerged columns using the same threshold terrain uses.
enum {
  SEA_LEVEL = 0,
  // Incremented by every change that alters generated terrain. Saved in
  // level.bin so a world from another generator is moved aside instead of
  // regenerating seams next to its saved chunks.
  WORLDGEN_VERSION = 6
};

typedef BlockID DenseBlockBuffer[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];

enum {
  PACKED_PALETTE_CAPACITY = 16,
  PACKED_INDEX_BITS = 4,
  PACKED_INDEX_MASK = 0xF,
  PACKED_CELLS_PER_BYTE = 2
};

// A chunk holding at most PACKED_PALETTE_CAPACITY distinct ids: a 4-bit index
// per cell into Palette. Z stays the contiguous axis, two cells per byte with
// the even Z in the low nibble, so a Z row is CHUNK_SIZE / 2 sequential bytes.
typedef struct {
  uint8_t Indices[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE / PACKED_CELLS_PER_BYTE];
  BlockID Palette[PACKED_PALETTE_CAPACITY];
  int PaletteCount;
} PackedBlockBuffer;

struct DenseBlockPool;

typedef struct Chunk {
  MeshHandle ChunkMesh;

  // Alpha-cutout geometry (flora) kept apart from the solid faces. Both want
  // depth writes, so they shared one mesh; they are split because a fragment
  // shader containing `discard` costs the hardware early depth rejection for the
  // whole draw, and only this geometry needs the test.
  MeshHandle CutoutMesh;

  MeshHandle TranslucentMesh;

  // Owned by the renderer, never read by the world: the chunk's 1-based entry
  // in the renderer's set of chunks holding a mesh, 0 when absent. Zero-filled
  // slots therefore start outside the set.
  int RenderRecord;

  // Storage is exactly one of: uniform (only UniformBlockId, IsUniform set),
  // packed (Packed, a palette of at most 16 ids), or dense (Blocks). Whole-chunk
  // writers fill Blocks; ChunkEndFill then compacts it to the smallest form.
  DenseBlockBuffer *Blocks;
  PackedBlockBuffer *Packed;
  // Owner of Blocks and Packed, and where promotion leases from. NULL for a
  // standalone chunk (tests, previews) whose dense buffer the caller attached
  // and owns; such a chunk is never compacted, so it is never packed.
  struct DenseBlockPool *Pool;
  BlockID UniformBlockId;
  bool IsUniform;

  // Derived from the seed and never saved to disk: FillChunkBiomeMap rebuilds it
  // for chunks loaded from disk exactly as it does for generated ones. Network
  // chunk packets do carry it, so clients can tint without knowing the seed.
  unsigned char BiomeMap[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK]
                        [BIOME_CELLS_PER_CHUNK];

  int ChunkX;
  int ChunkY;
  int ChunkZ;
  int SolidBlockCount;

  atomic_bool IsDirty;
  bool HasMesh;
  bool HasCutoutMesh;
  bool HasTranslucentMesh;
  bool IsModified;

  atomic_bool IsGenerating;
  atomic_bool IsGenerated;
  atomic_bool TerrainJustGenerated;

  // Mesh work is outstanding for this chunk on a worker thread. Eviction must
  // not recycle the slot while it is set: the worker holds a snapshot, not a
  // pointer, so it is not a memory-safety fault, but the returning mesh would
  // describe a position the chunk no longer occupies.
  atomic_bool IsMeshing;

  // Set when the chunk is edited after its snapshot was captured. The mesh in
  // flight is then stale, so completion leaves the chunk dirty and it is queued
  // again rather than the edit staying invisible.
  atomic_bool DirtiedWhileMeshing;

  // Owned by the chunk workers, under their queue lock: the next chunk in the
  // generation queue this one waits in. A chunk waits in at most one queue at a
  // time, so a link per chunk replaces a ring per worker sized for every chunk.
  struct Chunk *GenNext;
} Chunk;

// Resolve the base blocks that are not part of any biome palette (stone, water)
// from the block registry by name. Must run after the blocks are loaded and
// before the chunk workers start.
void InitTerrainGeneration(void);

struct ColumnEntry;

typedef enum {
  CUBE_CLASS_AIR = 0,
  CUBE_CLASS_SOLID,
  CUBE_CLASS_MIXED,
  CUBE_CLASS_COUNT
} CubeClass;

typedef struct {
  // Off forces every cube through the per-block path; only the self-test turns
  // it off, to prove the short-circuits change nothing.
  bool AllowClassification;
  // On for the chunk workers only, so the reported class counts describe the
  // world rather than previews and tests.
  bool CountClasses;
  // Self-test only: when non-NULL, receives the interpolated density of every
  // block (index x*256 + y*16 + z), NAN where none was evaluated.
  float *DebugDensity;
} TerrainGenOptions;

// Column is the cache entry for this chunk's (ChunkX, ChunkZ) and world seed.
void GenerateChunkTerrain(Chunk *ChunkVal, const struct ColumnEntry *Column);
// Returns the class the cube was classified as, whether or not the options
// allowed its short-circuit to be taken.
CubeClass GenerateChunkTerrainEx(Chunk *ChunkVal,
                                 const struct ColumnEntry *Column,
                                 const TerrainGenOptions *Options);

// Cubes generated by the chunk workers since startup, per class.
void GetCubeClassCounts(uint64_t Counts[CUBE_CLASS_COUNT]);

// Fill BiomeMap for this chunk's position. Call this for every chunk that
// becomes available for meshing, whether its blocks were generated or read back
// from disk — a loaded chunk has no biome map until this runs.
void FillChunkBiomeMap(Chunk *ChunkVal, const struct ColumnEntry *Column);

// Biome id of the cell containing a chunk-local block position.
unsigned char GetChunkBiomeAtLocal(const Chunk *ChunkVal, int LocalX,
                                   int LocalY, int LocalZ);

// Unchecked read of a chunk-local cell.
static inline BlockID ChunkGetLocal(const Chunk *ChunkVal, int LocalX,
                                    int LocalY, int LocalZ) {
  if (ChunkVal->Blocks != (DenseBlockBuffer *)0) {
    return (*ChunkVal->Blocks)[LocalX][LocalY][LocalZ];
  }
  const PackedBlockBuffer *Packed = ChunkVal->Packed;
  if (Packed != (PackedBlockBuffer *)0) {
    unsigned Byte = Packed->Indices[LocalX][LocalY][LocalZ >> 1];
    unsigned Shift = ((unsigned)LocalZ & 1U) * PACKED_INDEX_BITS;
    return Packed->Palette[(Byte >> Shift) & PACKED_INDEX_MASK];
  }
  return ChunkVal->UniformBlockId;
}

// Gives a standalone chunk a caller-owned buffer; it is never compacted.
void ChunkAttachBuffer(Chunk *ChunkVal, DenseBlockBuffer *Buffer);

// A zeroed chunk outside any world, with its own heap buffer (all air). NULL on
// allocation failure. Free it with ChunkFreeStandalone.
Chunk *ChunkCreateStandalone(void);
void ChunkFreeStandalone(Chunk *ChunkVal);

// Returns any leased buffer to the pool and makes the chunk uniform BlockId.
void ChunkResetStorage(Chunk *ChunkVal, BlockID BlockId);

// Whole-chunk writers (generation, deserialization) bracket their writes with
// these. Begin guarantees Blocks is writable, leasing a buffer if needed, and
// fails only when the pool cannot grow. End compacts a pooled chunk to uniform
// or packed storage when its cells allow it, releasing the dense buffer.
bool ChunkBeginFill(Chunk *ChunkVal);
void ChunkEndFill(Chunk *ChunkVal);

// Leases a dense buffer for a uniform or packed chunk and fills it with the
// cells it held. True when the chunk is dense afterwards.
bool PromoteChunkToDense(Chunk *ChunkVal);

void SetBlockInChunk(Chunk *ChunkVal, Vec3 Pos, BlockID BlockId);

// Block of a terrain fluid (CaveFluid value): air, water, or lava.
BlockID TerrainFluidBlock(int Fluid);
int GetBlockIdInChunk(Chunk *ChunkVal, Vec3 Pos);

#endif
