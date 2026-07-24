#ifndef FEATURE_H
#define FEATURE_H

#include "world/chunk.h"

// Deterministic, renderer-free pass that stamps prefab structures (currently
// trees) into a freshly generated chunk. It reads only seed-based global helpers
// and writes the passed chunk's own block data, so it is thread-safe by
// construction. Must run after GenerateChunkTerrain and never after a disk load.
//
// Cross-chunk structures are resolved by overlap-scan: every chunk independently
// re-derives each tree whose bounding box reaches into it and clips the stamp to
// its own bounds, so adjacent chunks produce complementary fragments with no
// seam and no dependence on load order. See design.md for the full contract.
void PlaceChunkFeatures(Chunk *ChunkVal);

#endif
