#ifndef WORLD_CHUNK_SNAPSHOT_H
#define WORLD_CHUNK_SNAPSHOT_H

#include "world/biome.h"
#include "world/chunk.h"

// An immutable copy of everything the mesher reads for one chunk.
//
// The mesher runs on worker threads, where dereferencing a live Chunk would
// race against generation, player edits and eviction. Handing it a snapshot
// instead makes that impossible by construction rather than by lock discipline:
// a worker that cannot reach a neighbouring chunk cannot observe it changing.
//
// Capture happens on the main thread, which is the only one that creates,
// evicts and edits chunks, so gathering needs no lock either.

// One block of margin on every face. Face culling reads the immediate
// neighbour, and the ambient-occlusion offsets reach diagonally to a corner, so
// chunk-local coordinates span -1..CHUNK_SIZE inclusive.
#define SNAPSHOT_BLOCK_SPAN (CHUNK_SIZE + 2)

// Tint interpolates across the four horizontal biome cells around a block. A
// block at the chunk's low edge resolves to cell 4*Cx-1 and one at the high edge
// to 4*Cx+3, and the interpolation also samples +1, giving six cells across X
// and Z. The vertical cell index is used directly with no neighbour sampling, so
// the chunk's own four cells are enough there.
#define SNAPSHOT_BIOME_SPAN_XZ (BIOME_CELLS_PER_CHUNK + 2)
#define SNAPSHOT_BIOME_SPAN_Y (BIOME_CELLS_PER_CHUNK)

typedef struct World World;

typedef struct {
  int ChunkX;
  int ChunkY;
  int ChunkZ;
  int SolidBlockCount;

  // Indexed [LocalX + 1][LocalY + 1][LocalZ + 1]. An absent neighbour stores 0,
  // which is what the live lookup returns for an unloaded chunk, so an
  // edge-of-world mesh is unchanged.
  unsigned char Blocks[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN]
                      [SNAPSHOT_BLOCK_SPAN];

  // Origin is cell (4*ChunkX - 1, 4*ChunkY, 4*ChunkZ - 1).
  unsigned char BiomeCells[SNAPSHOT_BIOME_SPAN_XZ][SNAPSHOT_BIOME_SPAN_Y]
                          [SNAPSHOT_BIOME_SPAN_XZ];

  // Presence is tracked separately rather than by a sentinel id: the live
  // lookup substitutes a caller-supplied fallback for an unloaded chunk, and
  // that fallback is the block's own cell, which varies per block and so cannot
  // be resolved at capture time.
  unsigned char BiomeCellPresent[SNAPSHOT_BIOME_SPAN_XZ][SNAPSHOT_BIOME_SPAN_Y]
                                [SNAPSHOT_BIOME_SPAN_XZ];
} ChunkSnapshot;

// Main thread only.
void CaptureChunkSnapshot(World *WorldVal, const Chunk *ChunkVal,
                          ChunkSnapshot *Out);

// Block at chunk-local coordinates, which may be -1 through CHUNK_SIZE.
unsigned char SnapshotBlockAt(const ChunkSnapshot *Snap, int LocalX, int LocalY,
                              int LocalZ);

// Biome id of a chunk-local cell, clamped like GetChunkBiomeAtLocal.
unsigned char SnapshotLocalBiome(const ChunkSnapshot *Snap, int LocalX,
                                 int LocalY, int LocalZ);

// Biome id of an absolute cell, returning Fallback where the owning chunk was
// not loaded — matching GetBiomeCellFromWorld.
unsigned char SnapshotBiomeCell(const ChunkSnapshot *Snap, int CellX, int CellY,
                                int CellZ, unsigned char Fallback);

#ifdef MINEGAME_SNAPSHOT_VERIFY
// Compare every cell the snapshot holds against the live lookup it replaced,
// returning the number of mismatches and logging the first few.
//
// This is the invariant the whole design rests on. The mesher's logic did not
// change when it moved to snapshot reads — only where the bytes come from — so
// "the mesh is unchanged" reduces to "the snapshot returns what the world
// returned". Checking that directly is stronger than comparing two meshes,
// because it covers every cell rather than only the ones a particular chunk's
// geometry happened to sample.
//
// Main thread only, and only meaningful before anything mutates the world.
int VerifyChunkSnapshot(World *WorldVal, const Chunk *ChunkVal,
                        const ChunkSnapshot *Snap);
#endif

#endif
