#ifndef WORLD_SURFACE_H
#define WORLD_SURFACE_H

#include "core/noise.h"
#include "core/vecmath.h"
#include <stdint.h>

// The preliminary surface: a pure 2D estimate of where a column's terrain
// surface lies. Biome Depth, structure and flora checks, spawn placement and the
// /biome command all read it, so none of them can drift from one another.
//
// While terrain is a heightmap it is exact. Once terrain becomes a 3D density
// field (overhangs, caves) it stays the 2D approximation those consumers need,
// and the real surface is whatever the density produces.
//
// Tuning lives in assets/worldgen/noise.json; the built-in defaults below apply
// when the file is missing or a field is absent.

typedef struct {
  NoiseFieldDesc Field;
  float BaseHeight;
  float Amplitude;
} SurfaceParams;

typedef struct {
  int X;
  int Z;
  int Surface;
  Vec3 FeetPosition;
} SpawnPoint;

// Defines the surface field with built-in defaults. Must run before
// LoadSurfaceParams and before the chunk workers start.
void InitSurfaceParams(void);

// Overrides the defaults from JSON; keeps the defaults for anything missing.
void LoadSurfaceParams(const char *FilePath);

const SurfaceParams *GetSurfaceParams(void);

int GetPreliminarySurface(int GlobalX, int GlobalZ, uint64_t Seed);

// First column above sea level in a fixed square spiral from (0, 0), searched up
// to SPAWN_SEARCH_RADIUS; (0, 0) if none qualifies. Deterministic per seed.
SpawnPoint FindSpawnColumn(uint64_t Seed);

#endif
