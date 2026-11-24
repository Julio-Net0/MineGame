#ifndef RENDER_VOXEL_MESHER_H
#define RENDER_VOXEL_MESHER_H

#include "render/backend.h"
#include "world/chunk_snapshot.h"
#include "world/lod/lod_section.h"
#include <stdbool.h>
#include <stdint.h>

// The greedy cube mesher, shared by real chunks and LOD sections.
//
// Its input is always the flat 18^3 block grid of a ChunkSnapshot (16^3 cells
// plus a one-cell margin), read directly in the hot loops with no per-cell
// indirection. A LOD section is meshed by laying its voxels out in that same
// grid; the only difference reaches the mesher per visible face, not per
// cell: its tint comes from the voxel's own biome instead of the four-cell
// biome blend a chunk uses.

// Which of a chunk's three meshes a face belongs to.
//
// Solid cube faces and alpha-cutout flora used to share one buffer, because both
// want depth writes and depth testing on. They are separated because they want
// different fragment shaders: a shader containing `discard` costs the hardware
// its early depth rejection for the whole draw, and only the cutout geometry
// needs the test. Merged, every chunk carrying a single flower would forfeit
// early-Z across all of its solid geometry.
typedef enum {
  MESH_TARGET_OPAQUE = 0,
  MESH_TARGET_CUTOUT,
  MESH_TARGET_TRANSLUCENT,
  MESH_TARGET_COUNT
} MeshTarget;

// Backend-agnostic mesh-builder scratch for one target. Filled per chunk, then
// handed to the render backend as MeshData. Holds no graphics-API types.
typedef struct {
  ChunkVertex Vertices[CHUNK_MAX_VERTICES];
  int VCount;
} MeshScratch;

// Face-culling classes of every snapshot cell, packed as bit columns so the
// mesher finds a slice's visible faces with a few word operations instead of a
// block lookup per cell. Bit k of a column is snapshot index k along its axis
// (chunk-local k - 1), so the one-block margin sits in bits 0 and
// SNAPSHOT_BLOCK_SPAN - 1. Only the Z and Y axes are kept: every face direction
// lays its slice rows along one of them.
typedef struct {
  // Indexed [x][y], bits along z.
  uint32_t EmitsZ[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  uint32_t PassesZ[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  uint32_t TransparentZ[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  // Indexed [x][z], bits along y.
  uint32_t EmitsY[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  uint32_t PassesY[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
  uint32_t TransparentY[SNAPSHOT_BLOCK_SPAN][SNAPSHOT_BLOCK_SPAN];
} CullColumns;

typedef struct {
  // All three sized alike. The cutout set will hold far less than the others in
  // practice, but a shared size keeps every emit path identical and removes a
  // class of bug where a helper indexes the wrong capacity.
  MeshScratch Sets[MESH_TARGET_COUNT];
  CullColumns Cull;
  // One biome per interior cell, for LOD sections; NULL for chunks, whose tint
  // blends the snapshot's biome cells.
  const struct VoxelLodBiomes *LodBiomes;
  // A target filled up and faces were dropped.
  bool Truncated;
} VoxelMesher;

// Resolves the per-block culling classes. Main thread, after the block
// registry is final and before any meshing.
void VoxelMesherInit(void);

// Meshes one chunk: greedy cube faces, faces of unloaded rock beside it, and
// cross billboards. Worker-safe: reads only Snap and writes only Mesher.
void VoxelMeshChunk(VoxelMesher *Mesher, const ChunkSnapshot *Snap);

enum {
  // Horizontal margin of neighbour biomes around a section, in voxels: what
  // a level-1 voxel's tint blend reads (see VoxelMeshLodSection).
  VOXEL_LOD_BIOME_MARGIN = 2,
  VOXEL_LOD_BIOME_SPAN = CHUNK_SIZE + (2 * VOXEL_LOD_BIOME_MARGIN)
};

typedef struct VoxelLodBiomes {
  // [X + MARGIN][Y][Z + MARGIN]: the section's voxel biomes and, around them,
  // the nearest layers of its four side neighbours (its own edge repeated
  // where a neighbour is absent, and at the corners).
  unsigned char Cells[VOXEL_LOD_BIOME_SPAN][CHUNK_SIZE][VOXEL_LOD_BIOME_SPAN];
  // Level 1 blends tints across biome cells exactly as chunks do; the
  // section's world origin in blocks places the cells.
  bool Blend;
  int OriginX;
  int OriginZ;
} VoxelLodBiomes;

// Workspace for meshing a LOD section: its voxels laid out as a snapshot grid
// and their biomes. About 32 KB; one per meshing thread.
typedef struct {
  ChunkSnapshot Grid;
  LodVoxel Voxels[LOD_SECTION_VOLUME];
  VoxelLodBiomes Biomes;
} VoxelLodScratch;

// Neighbours of a section on its own level, indexed like ChunkFace (-X, +X,
// -Y, +Y, -Z, +Z); NULL where none is resident.
enum {
  VOXEL_LOD_NEIGHBOURS = 6
};

// Where a section sits: its level and section coordinates on that level.
typedef struct {
  int Level;
  int X;
  int Y;
  int Z;
} VoxelLodPlace;

// Meshes a LOD section (any level) into the same 8-byte vertices as a chunk,
// in section-local voxel units: the caller scales by 2^L. Neighbours fill the
// one-voxel margin, so faces between two resident sections are culled and AO
// reads across the border; an absent neighbour reads as air. Section voxels
// hold no cross plants (they downsample to air), so only cube faces come out.
// The LOD draws see-through blocks (water, ice, glass: textures with partial
// alpha or mostly transparent, see RenderBlockLayerSeeThrough) blended, from
// the translucent target; every other face, leaves included, goes to the
// opaque target. Level-1 tints blend across biome cells like a chunk's, so
// the LOD next to the 1:1 chunks carries the colour of the blocks it stands
// for; coarser levels take each voxel's own biome.
// TransitionFaces (bit per face, ChunkFace order) marks neighbours drawn at
// another level: their faces are meshed as if the neighbour held this
// section's own see-through blocks and air elsewhere (see FillTransitionMargin).
void VoxelMeshLodSection(VoxelMesher *Mesher, VoxelLodScratch *Scratch,
                         const LodSection *Section, const VoxelLodPlace *Place,
                         const LodSection *const Neighbours[VOXEL_LOD_NEIGHBOURS],
                         unsigned TransitionFaces);

#endif
