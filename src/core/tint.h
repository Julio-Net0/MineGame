#ifndef CORE_TINT_H
#define CORE_TINT_H

// Which biome color palette a block's faces are multiplied by.
//
// This lives in core/ rather than in block_system.h or biome.h because both need
// to name it: the block registry stores which source a block declares, and the
// biome registry stores the colors each source resolves to. Putting it in either
// one would force that layer to include the other.
//
// TINT_GRASS resolves to the biome's grass color and applies to the top face
// and side overlay only, because the grass side base texture contains dirt.
// TINT_FOLIAGE and TINT_WATER resolve to the biome's foliage and water colors
// and apply to every face.
typedef enum {
  TINT_NONE = 0,
  TINT_GRASS,
  TINT_FOLIAGE,
  TINT_WATER
} TintSource;

#endif
