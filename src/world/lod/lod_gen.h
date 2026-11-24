#ifndef WORLD_LOD_LOD_GEN_H
#define WORLD_LOD_LOD_GEN_H

#include "world/lod/lod_section.h"
#include <stdbool.h>
#include <stdint.h>

// Synthetic LOD terrain: level-L sections straight from the world generator's
// pure functions, without generating a single chunk. A voxel at level L stands
// for a 2^L block cube and is meant to match what downsampling the real chunks
// would give it:
//
//   - Heights come from the density field's terrain top at four corner
//     columns per voxel column, which is solid up to the highest of them: the
//     sampled stand-in for "any solid member wins". Up to level 3 they are
//     the corners of the voxel's own footprint (exact at level 1); coarser
//     levels share corners with their neighbours, at a quarter of the cost.
//   - The voxel holding the top takes the biome's surface layer (underwater
//     and steep variants included), the way an exposed top member wins the
//     downsample; voxels under it take the layer at their top row, then the
//     stone filler.
//   - Above the top, voxels wholly at or below the sea level are water, the
//     highest of them sea ice in frozen biomes; the rest is air. Unlike the
//     downsample (any water member wins), this keeps the sea surface at one
//     height on every level, so levels meet over water without a crack.
//
// No caves beyond the top, no structures, trees or flora. Everything is a pure
// function of (level, position, seed), so a column is the same whichever
// thread generates it and in whatever order.
//
// A column is the 16 x 16 voxel footprint of one section across every height
// that holds something visible: from just below its lowest exposed voxel
// (neighbouring columns included, so cliff walls are whole) to its highest
// solid or water voxel. Air-only sections are never emitted.
//
// Each emitted section carries the 4^3 cells worth drawing as present: cells
// holding water or a solid voxel with a non-solid neighbour. Buried rock is
// stored but not present, so nothing refines into it or draws it.
//
// Thread-safe given one scratch per thread, once the block registry, the
// worldgen data and LodDownsampleInit are loaded.

enum {
  // Sections one column can emit: the overworld's full height at level 1
  // with room to spare.
  LOD_GEN_MAX_SECTIONS = 48
};

typedef struct LodGenScratch LodGenScratch;

// About 32 KB of samples and voxels; NULL when out of memory.
LodGenScratch *LodGenScratchCreate(void);
void LodGenScratchFree(LodGenScratch *Scratch);

// Receives each emitted section, bottom to top. Voxels is the scratch's buffer,
// valid only during the call.
typedef void (*LodGenEmit)(void *User, int SectionY,
                           const LodVoxel Voxels[LOD_SECTION_VOLUME],
                           uint64_t PresentCells);

typedef struct {
  // Section rows considered, inclusive; FirstY > LastY when none.
  int FirstY;
  int LastY;
  int Emitted;
  int AirSkipped;
  // Terrain-top probes and biome samples taken.
  int TopSamples;
  int BiomeSamples;
} LodGenResult;

// Generates the level-Level section column (X, Z), in level-Level section
// units, calling Emit for every section that holds anything.
void LodGenerateColumn(LodGenScratch *Scratch, int Level, int X, int Z,
                       uint64_t Seed, LodGenEmit Emit, void *User,
                       LodGenResult *Out);

#endif
