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
// only, because the grass side texture is a pre-baked dirt-and-fringe image that
// would discolor if tinted whole. TINT_FOLIAGE resolves to the biome's foliage
// color and applies to every face.
typedef enum {
  TINT_NONE = 0,
  TINT_GRASS,
  TINT_FOLIAGE
} TintSource;

#endif
