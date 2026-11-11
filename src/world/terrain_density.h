#ifndef WORLD_TERRAIN_DENSITY_H
#define WORLD_TERRAIN_DENSITY_H

#include <stdint.h>

// Overworld terrain as a 3D density field, in blocks:
//
//   d(x,y,z) = Base(x,z) - y + Variation(x,z) * n3(x, y*k, z) - Taper(y)
//
// A position is solid where d > 0. Density is evaluated only at grid corners
// (every 4 blocks in X and Z, every 8 in Y, at global lattice positions) and
// every block is the trilinear blend of its cell's 8 corners. The generator and
// the pure probe below share the corner function and the blend, so the probe
// returns exactly the float the generator used.

enum {
  DENSITY_CELL_XZ = 4,
  DENSITY_CELL_Y = 8,
  // Corners per cube: 5 x 4 x 5, the 4th Y row lying 8 blocks above the cube
  // top so the palette pass can see what sits above it.
  DENSITY_CORNERS_XZ = 5,
  DENSITY_CORNERS_Y = 4,
  // Rows above the cube the palette pass reads through the extra corner row.
  DENSITY_TOP_PADDING = 8,
  DENSITY_CELL_CORNERS = 8
};

// Density at a lattice corner (X, Z multiples of 4, Y a multiple of 8) given the
// column shape there.
float TerrainCornerDensity(float Base, float Variation, int X, int Y, int Z,
                           uint64_t Seed);

// Trilinear blend of a cell's corners. Corner index = x + 2*y + 4*z, each 0 or
// 1. One definition, so every caller performs the same operations in the same
// order.
static inline float TrilinearDensity(const float Corners[DENSITY_CELL_CORNERS],
                                     float Fx, float Fy, float Fz) {
  float X00 = Corners[0] + ((Corners[1] - Corners[0]) * Fx);
  float X10 = Corners[2] + ((Corners[3] - Corners[2]) * Fx);
  float X01 = Corners[4] + ((Corners[5] - Corners[4]) * Fx);
  float X11 = Corners[6] + ((Corners[7] - Corners[6]) * Fx);
  float Y0 = X00 + ((X10 - X00) * Fy);
  float Y1 = X01 + ((X11 - X01) * Fy);
  return Y0 + ((Y1 - Y0) * Fz);
}

// Interpolated overworld density at a block, bit-identical to what terrain
// generation computes for it.
float SampleTerrainDensity(int X, int Y, int Z, uint64_t Seed);

// Y of the highest overworld block of the column with density > 0, ignoring
// structures and flora; one below the overworld floor if the column has none.
int GetTerrainTop(int X, int Z, uint64_t Seed);

#endif
