#ifndef WORLD_LOD_LOD_DOWNSAMPLE_H
#define WORLD_LOD_LOD_DOWNSAMPLE_H

#include "world/chunk_snapshot.h"
#include "world/lod/lod_section.h"
#include "world/lod/lod_voxel.h"

// Halving a 16^3 grid of voxels into an 8^3 patch, one output voxel per 2x2x2
// group. The same rule turns a chunk into its level-1 patch and a level-L
// section into its share of the level-(L+1) section above it.
//
// The rule reads nothing but the 16^3 input, never a neighbouring chunk or
// section, so a patch is a pure function of its own source and the result of
// ingesting many chunks does not depend on their order. Thread-safe once
// LodDownsampleInit has run.

// Visibility classes, in priority order: a group containing any voxel of a
// higher class is represented by a voxel of that class. Cross-shaped plants
// fall to AIR, since a 2^L block cube of flowers would only add noise.
typedef enum {
  LOD_CLASS_AIR = 0,
  // See-through solid cubes: leaves, glass, ice.
  LOD_CLASS_LEAVES,
  // Translucent non-solid cubes: water.
  LOD_CLASS_WATER,
  LOD_CLASS_OPAQUE,
  LOD_CLASS_COUNT
} LodClass;

enum {
  LOD_PATCH_SIZE = LOD_SECTION_SIZE / 2,
  LOD_PATCH_VOLUME = LOD_PATCH_SIZE * LOD_PATCH_SIZE * LOD_PATCH_SIZE
};

// [X][Y][Z], Z contiguous; 2 KB.
typedef struct {
  LodVoxel Voxels[LOD_PATCH_VOLUME];
} LodPatch8;

static inline int LodPatchIndex(int X, int Y, int Z) {
  return (X * LOD_PATCH_SIZE * LOD_PATCH_SIZE) + (Y * LOD_PATCH_SIZE) + Z;
}

// Builds the class of every registered block id from its definition. Main
// thread, after the block registry is loaded and before any downsampling.
void LodDownsampleInit(void);

LodClass LodClassifyBlock(BlockID Id);

// The voxel a block becomes: air-class blocks collapse to LOD_VOXEL_AIR.
LodVoxel LodCanonicalVoxel(BlockID Id, unsigned Biome);

// Level 0 -> 1: the snapshot's own 16^3 cells, margins ignored.
void LodDownsampleSnapshot(const ChunkSnapshot *Snap, LodPatch8 *Out);

// Level 0 -> 1 for a chunk held as one block id (all air, or uniform rock
// the mesher skips): the patch LodDownsampleSnapshot would give, without a
// snapshot. BiomeMap is the chunk's own.
void LodDownsampleUniformChunk(
    BlockID Id,
    const unsigned char BiomeMap[BIOME_CELLS_PER_CHUNK][BIOME_CELLS_PER_CHUNK]
                                [BIOME_CELLS_PER_CHUNK],
    LodPatch8 *Out);

void LodPatchFill(LodPatch8 *Out, LodVoxel Value);

// Level L -> L+1.
void LodDownsampleSection(const LodSection *Section, LodPatch8 *Out);

// The rule itself, on canonical voxels indexed like a section.
void LodDownsampleVoxels(const LodVoxel In[LOD_SECTION_VOLUME], LodPatch8 *Out);

#endif
