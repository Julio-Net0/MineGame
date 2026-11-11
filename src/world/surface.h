#ifndef WORLD_SURFACE_H
#define WORLD_SURFACE_H

#include "core/vecmath.h"
#include <stdint.h>

// The preliminary surface: a pure 2D estimate of where a column's terrain
// surface lies. Biome Depth, structure and flora checks, spawn placement and the
// /biome command all read it, so none of them can drift from one another.
//
// It is the bilinear blend of the terrain shape's Base between the four
// surrounding lattice columns: pure, cheap, and cached. Where the 3D terrain
// field displaces the surface (overhangs, cliffs) it is only the 2D
// approximation those consumers need; the real surface is whatever the density
// produces.

typedef struct {
  int X;
  int Z;
  // Terrain top of the spawn column (GetTerrainTop), not the preliminary
  // surface: the player has to land on what was actually generated.
  int Surface;
  Vec3 FeetPosition;
} SpawnPoint;

int GetPreliminarySurface(int GlobalX, int GlobalZ, uint64_t Seed);

// First column whose terrain top is above sea level, in a fixed square spiral
// from (0, 0) searched up to SPAWN_SEARCH_RADIUS; (0, 0) if none qualifies.
// Deterministic per seed.
SpawnPoint FindSpawnColumn(uint64_t Seed);

#endif
