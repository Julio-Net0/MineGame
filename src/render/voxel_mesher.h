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
  // LOD sections only: voxels between two chunk column planes, where the
  // section is cut against the 1:1 chunks and closes the cut with skirt faces
  // (see BuildLodSkirtPass); 0 emits none.
  int LodSkirtPeriod;
  // LOD sections the border pass draws: voxels between two chunk column
  // planes, where greedy quads stop (see VoxelMeshLodSection); 0 merges freely.
  int LodPlanePeriod;
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

// The 26 sections around a section on its own level, at index
// VoxelLodAround(Dx, Dy, Dz) for offsets in -1..1 (the centre slot is unused);
// NULL where none is resident.
enum {
  VOXEL_LOD_AROUND_SPAN = 3,
  VOXEL_LOD_AROUND_PLANE = VOXEL_LOD_AROUND_SPAN * VOXEL_LOD_AROUND_SPAN,
  VOXEL_LOD_AROUND = VOXEL_LOD_AROUND_PLANE * VOXEL_LOD_AROUND_SPAN
};
static inline int VoxelLodAround(int Dx, int Dy, int Dz) {
  return ((Dx + 1) * VOXEL_LOD_AROUND_PLANE) + ((Dy + 1) * VOXEL_LOD_AROUND_SPAN) +
         (Dz + 1);
}

// Where a section sits: its level and section coordinates on that level.
typedef struct {
  int Level;
  int X;
  int Y;
  int Z;
  // Mesh skirt faces (levels 1..VOXEL_LOD_SKIRT_MAX_LEVEL only) and end quads
  // on chunk column planes: sections the border pass draws ask for them.
  bool Skirts;
} VoxelLodPlace;

// Meshes a LOD section (any level) into the same 8-byte vertices as a chunk,
// in section-local voxel units: the caller scales by 2^L. The sections Around
// fill the one-voxel margin, edges and corners included, so faces between two
// resident sections are culled and AO reads across every border; an absent
// neighbour reads as air. Section voxels
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
// Two LOD-only vertex flags use the packed vertex's spare top bits (lod.vert
// reads them): VOXEL_LOD_FLAG_SKIRT on skirt faces, drawn only where the
// section is cut against the 1:1 chunks, and VOXEL_LOD_FLAG_SEA on water and
// sea-ice faces, whose sea-level lines the shader moves onto the 1:1 sea.
// Skirts are meshed up to this level (see VoxelMeshLodSection).
enum { VOXEL_LOD_SKIRT_MAX_LEVEL = 3 };
#define VOXEL_LOD_FLAG_SKIRT (UINT32_C(1) << 30U)
#define VOXEL_LOD_FLAG_SEA (UINT32_C(1) << 31U)
void VoxelMeshLodSection(VoxelMesher *Mesher, VoxelLodScratch *Scratch,
                         const LodSection *Section, const VoxelLodPlace *Place,
                         const LodSection *const Around[VOXEL_LOD_AROUND],
                         unsigned TransitionFaces);

#endif
