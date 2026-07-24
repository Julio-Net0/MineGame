#ifndef FEATURE_H
#define FEATURE_H

#include "world/chunk.h"

// Deterministic, renderer-free pass that stamps prefab structures into a freshly
// generated chunk. The prefab for each candidate column is drawn from the
// weighted structure set of the biome resolved at that column, so a bare biome
// places nothing. It reads only seed-based global helpers and the read-only
// biome/prefab registries and writes the passed chunk's own block data, so it is
// thread-safe by construction. Must run after GenerateChunkTerrain and never
// after a disk load.
//
// Cross-chunk structures are resolved by overlap-scan: every chunk independently
// re-derives each structure whose bounding box reaches into it and clips the
// stamp to its own bounds, so adjacent chunks produce complementary fragments
// with no seam and no dependence on load order. See design.md for the full
// contract.
void PlaceChunkFeatures(Chunk *ChunkVal);

// Scatter single-block, biome-driven flora across this chunk's surface. Like
// PlaceChunkFeatures it is deterministic in (worldX, worldZ, seed), writes only
// this chunk's block data over air, and must run on the generate-only path. A
// flora block is placed one voxel above the surface when the biome declares a
// flora set, the surface is above sea level and is the biome's own surface
// block, and both the surface and the voxel above it lie inside this chunk.
void PlaceChunkFlora(Chunk *ChunkVal);

#endif
