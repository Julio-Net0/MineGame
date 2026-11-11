#ifndef FEATURE_H
#define FEATURE_H

#include "world/chunk.h"
#include <stdbool.h>

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
// Candidate surfaces come from Cache (the generating thread's column cache).
// AllowSkip lets the pass return early for a cube no candidate can reach; only
// the self-test turns it off, to prove skipping changes nothing.
struct ColumnCache;
void PlaceChunkFeatures(Chunk *ChunkVal, struct ColumnCache *Cache,
                        bool AllowSkip);

// First structure the pass would place, scanning grid cells within CellRadius
// of the origin in a fixed order; its stamp origin (the cell above the anchor
// surface) is returned. Lets tests aim at a cube that really holds a structure.
bool FindStructureAnchor(int CellRadius, uint64_t Seed, int *OutX, int *OutY,
                         int *OutZ);

// Like FindStructureAnchor, but only accepts a structure whose terrain-top
// anchor lies at least MinGap blocks from its column's preliminary surface, so
// tests can aim at one the 3D terrain displaced.
bool FindDisplacedStructureAnchor(int CellRadius, int MinGap, uint64_t Seed,
                                  int *OutX, int *OutY, int *OutZ);

// The structure the pass would place for the grid cell containing column
// (WorldX, WorldZ), from the pure path: false when the cell places nothing.
// For tools that tally vegetation by biome without generating chunks.
bool ResolveStructureAt(int WorldX, int WorldZ, uint64_t Seed, int *OutBiomeId,
                        const char **OutPrefabName);

// The terrain top the structure pass anchors at, through its per-thread memo.
int FeatureTerrainTop(int WorldX, int WorldZ, uint64_t Seed);

// Scatter single-block, biome-driven flora across this chunk's surface. Like
// PlaceChunkFeatures it is deterministic in (worldX, worldZ, seed), writes only
// this chunk's block data over air, and must run on the generate-only path. A
// flora block is placed one voxel above the surface when the biome declares a
// flora set, the surface is above sea level and is the biome's own surface
// block, and both the surface and the voxel above it lie inside this chunk.
// Column is the cache entry for this chunk's column; the surface comes from it.
void PlaceChunkFlora(Chunk *ChunkVal, const struct ColumnEntry *Column);

#endif
