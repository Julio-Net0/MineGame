#ifndef WORLD_LOD_LOD_VOXEL_H
#define WORLD_LOD_LOD_VOXEL_H

#include "world/block_id.h"
#include <stdint.h>

// One cell of the level-of-detail hierarchy, packed in 32 bits:
//   bits  0-15  BlockID
//   bits 16-23  biome id
//   bits 24-27  sky light (0-15)
//   bits 28-31  block light (0-15)
//
// The engine has no light propagation yet, so every voxel carries the defaults
// (full sky light, no block light); the bits are reserved so adding lighting
// later does not change the format. Air is canonical: every air voxel is the
// same value, whatever biome the cells it replaces had, so empty regions
// compact to a single uniform value.

typedef uint32_t LodVoxel;

_Static_assert(sizeof(LodVoxel) == 4, "LodVoxel must stay 32 bits");
_Static_assert(sizeof(BlockID) == 2, "LodVoxel packs BlockID in 16 bits");

enum {
  LOD_VOXEL_BIOME_SHIFT = 16,
  LOD_VOXEL_SKY_SHIFT = 24,
  LOD_VOXEL_BLOCK_LIGHT_SHIFT = 28,
  LOD_VOXEL_LIGHT_MAX = 15,
  LOD_VOXEL_DEFAULT_SKY_LIGHT = 15,
  LOD_VOXEL_DEFAULT_BLOCK_LIGHT = 0
};

#define LOD_VOXEL_ID_MASK 0xFFFFU
#define LOD_VOXEL_BIOME_MASK 0xFFU
#define LOD_VOXEL_LIGHT_MASK 0xFU

static inline LodVoxel LodVoxelPack(BlockID Id, unsigned Biome, unsigned SkyLight,
                                    unsigned BlockLight) {
  return (LodVoxel)Id |
         ((LodVoxel)(Biome & LOD_VOXEL_BIOME_MASK) << LOD_VOXEL_BIOME_SHIFT) |
         ((LodVoxel)(SkyLight & LOD_VOXEL_LIGHT_MASK) << LOD_VOXEL_SKY_SHIFT) |
         ((LodVoxel)(BlockLight & LOD_VOXEL_LIGHT_MASK)
          << LOD_VOXEL_BLOCK_LIGHT_SHIFT);
}

// A solid, fluid or see-through voxel with the default light.
static inline LodVoxel LodVoxelMake(BlockID Id, unsigned Biome) {
  return LodVoxelPack(Id, Biome, LOD_VOXEL_DEFAULT_SKY_LIGHT,
                      LOD_VOXEL_DEFAULT_BLOCK_LIGHT);
}

#define LOD_VOXEL_AIR                                                         \
  ((LodVoxel)((LodVoxel)LOD_VOXEL_DEFAULT_SKY_LIGHT << LOD_VOXEL_SKY_SHIFT))

static inline BlockID LodVoxelBlock(LodVoxel Voxel) {
  return (BlockID)(Voxel & LOD_VOXEL_ID_MASK);
}

static inline unsigned LodVoxelBiome(LodVoxel Voxel) {
  return (Voxel >> LOD_VOXEL_BIOME_SHIFT) & LOD_VOXEL_BIOME_MASK;
}

static inline unsigned LodVoxelSkyLight(LodVoxel Voxel) {
  return (Voxel >> LOD_VOXEL_SKY_SHIFT) & LOD_VOXEL_LIGHT_MASK;
}

static inline unsigned LodVoxelBlockLight(LodVoxel Voxel) {
  return (Voxel >> LOD_VOXEL_BLOCK_LIGHT_SHIFT) & LOD_VOXEL_LIGHT_MASK;
}

#endif
