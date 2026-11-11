#ifndef WORLD_TERRAIN_DENSITY_H
#define WORLD_TERRAIN_DENSITY_H

#include "world/cave_fields.h"
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
// column shape there, caves included.
float TerrainCornerDensity(float Base, float Variation, float ShellDepth, int X,
                           int Y, int Z, uint64_t Seed);

// Cell corner indices, x + 2*y + 4*z with each of x, y, z 0 or 1.
enum {
  CELL_X0Y0Z0 = 0,
  CELL_X1Y0Z0 = 1,
  CELL_X0Y1Z0 = 2,
  CELL_X1Y1Z0 = 3,
  CELL_X0Y0Z1 = 4,
  CELL_X1Y0Z1 = 5,
  CELL_X0Y1Z1 = 6,
  CELL_X1Y1Z1 = 7
};

// Trilinear blend of a cell's corners. One definition, so every caller
// performs the same operations in the same order.
static inline float TrilinearDensity(const float Corners[DENSITY_CELL_CORNERS],
                                     float Fx, float Fy, float Fz) {
  float X00 = Corners[CELL_X0Y0Z0] +
              ((Corners[CELL_X1Y0Z0] - Corners[CELL_X0Y0Z0]) * Fx);
  float X10 = Corners[CELL_X0Y1Z0] +
              ((Corners[CELL_X1Y1Z0] - Corners[CELL_X0Y1Z0]) * Fx);
  float X01 = Corners[CELL_X0Y0Z1] +
              ((Corners[CELL_X1Y0Z1] - Corners[CELL_X0Y0Z1]) * Fx);
  float X11 = Corners[CELL_X0Y1Z1] +
              ((Corners[CELL_X1Y1Z1] - Corners[CELL_X0Y1Z1]) * Fx);
  float Y0 = X00 + ((X10 - X00) * Fy);
  float Y1 = X01 + ((X11 - X01) * Fy);
  return Y0 + ((Y1 - Y0) * Fz);
}

// Overworld density at a block, flood barriers included, bit-identical to what
// terrain generation computes for it.
float SampleTerrainDensity(int X, int Y, int Z, uint64_t Seed);

// Fluid of a non-solid overworld position below the ceiling, decided exactly
// as terrain generation decides it. Meaningless where the density is positive.
CaveFluid SampleTerrainFluid(int X, int Y, int Z, uint64_t Seed);

// Y of the highest overworld block of the column with density > 0, ignoring
// structures and flora; one below the overworld floor if the column has none.
int GetTerrainTop(int X, int Z, uint64_t Seed);

#endif
