#include "world/terrain_density.h"
#include "world/terrain_shape.h"
#include "world/worldgen_strata.h"
#include <limits.h>
#include <math.h>

static int FloorToMultiple(int Value, int Step) {
  int Quotient = Value / Step;
  if ((Value % Step != 0) && (Value < 0)) {
    Quotient--;
  }
  return Quotient * Step;
}

// Smoothly pushes density down over the last T blocks below the generation
// ceiling, so a peak that would reach it is rounded instead of cut flat.
static float CeilingTaper(int Y) {
  float Taper = GetCeilingTaper();
  float Over = (float)Y - ((float)GetGenerationCeiling() - Taper);
  return (Over > 0.0F) ? ((Over * Over) / Taper) : 0.0F;
}

float TerrainCornerDensity(float Base, float Variation, int X, int Y, int Z,
                           uint64_t Seed) {
  float Density = Base - (float)Y;
  if (Variation > 0.0F) {
    Density += Variation * SampleTerrainNoise3D(X, Y, Z, Seed);
  }
  return Density - CeilingTaper(Y);
}

// The four lattice-column shapes around a block column, evaluated by the same
// function the column cache uses.
typedef struct {
  ColumnShape Shapes[2][2];
  int X0;
  int Z0;
} ProbeColumn;

static void LoadProbeColumn(int X, int Z, uint64_t Seed, ProbeColumn *Out) {
  Out->X0 = FloorToMultiple(X, DENSITY_CELL_XZ);
  Out->Z0 = FloorToMultiple(Z, DENSITY_CELL_XZ);
  for (int Ix = 0; Ix < 2; Ix++) {
    for (int Iz = 0; Iz < 2; Iz++) {
      Out->Shapes[Ix][Iz] =
          EvaluateColumnShape(Out->X0 + (Ix * DENSITY_CELL_XZ),
                              Out->Z0 + (Iz * DENSITY_CELL_XZ), Seed);
    }
  }
}

static void ProbeCellCorners(const ProbeColumn *Column, int Y0, uint64_t Seed,
                             float Corners[DENSITY_CELL_CORNERS]) {
  for (int Iz = 0; Iz < 2; Iz++) {
    for (int Iy = 0; Iy < 2; Iy++) {
      for (int Ix = 0; Ix < 2; Ix++) {
        const ColumnShape *Shape = &Column->Shapes[Ix][Iz];
        Corners[Ix + (2 * Iy) + (4 * Iz)] = TerrainCornerDensity(
            Shape->Base, Shape->Variation, Column->X0 + (Ix * DENSITY_CELL_XZ),
            Y0 + (Iy * DENSITY_CELL_Y), Column->Z0 + (Iz * DENSITY_CELL_XZ),
            Seed);
      }
    }
  }
}

float SampleTerrainDensity(int X, int Y, int Z, uint64_t Seed) {
  ProbeColumn Column;
  LoadProbeColumn(X, Z, Seed, &Column);
  int Y0 = FloorToMultiple(Y, DENSITY_CELL_Y);

  float Corners[DENSITY_CELL_CORNERS];
  ProbeCellCorners(&Column, Y0, Seed, Corners);
  return TrilinearDensity(Corners,
                          (float)(X - Column.X0) / (float)DENSITY_CELL_XZ,
                          (float)(Y - Y0) / (float)DENSITY_CELL_Y,
                          (float)(Z - Column.Z0) / (float)DENSITY_CELL_XZ);
}

int GetTerrainTop(int X, int Z, uint64_t Seed) {
  int Floor = INT_MIN;
  int Unused = 0;
  if (!GetStratumRange(STRATUM_GEN_OVERWORLD, &Floor, &Unused)) {
    return INT_MIN;
  }

  ProbeColumn Column;
  LoadProbeColumn(X, Z, Seed, &Column);

  // Nothing can be solid above max(Base + bound * Variation): start there
  // rather than at the ceiling.
  float Highest = -INFINITY;
  for (int Ix = 0; Ix < 2; Ix++) {
    for (int Iz = 0; Iz < 2; Iz++) {
      const ColumnShape *Shape = &Column.Shapes[Ix][Iz];
      Highest = fmaxf(Highest, Shape->Base + (TERRAIN_NOISE_BOUND * Shape->Variation));
    }
  }
  int Start = GetGenerationCeiling() - 1;
  if (Highest < (float)Start) {
    Start = (int)floorf(Highest);
  }

  float Fx = (float)(X - Column.X0) / (float)DENSITY_CELL_XZ;
  float Fz = (float)(Z - Column.Z0) / (float)DENSITY_CELL_XZ;
  int Y = Start;
  while (Y >= Floor) {
    int Y0 = FloorToMultiple(Y, DENSITY_CELL_Y);
    float Corners[DENSITY_CELL_CORNERS];
    ProbeCellCorners(&Column, Y0, Seed, Corners);
    int Stop = (Y0 > Floor) ? Y0 : Floor;
    for (int Block = Y; Block >= Stop; Block--) {
      float Fy = (float)(Block - Y0) / (float)DENSITY_CELL_Y;
      if (TrilinearDensity(Corners, Fx, Fy, Fz) > 0.0F) {
        return Block;
      }
    }
    Y = Y0 - 1;
  }
  return Floor - 1;
}
