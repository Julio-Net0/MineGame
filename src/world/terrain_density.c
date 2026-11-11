#include "world/terrain_density.h"
#include "world/cave_fields.h"
#include "world/surface.h"
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

float TerrainCornerDensity(float Base, float Variation, float ShellDepth, int X,
                           int Y, int Z, uint64_t Seed) {
  float Density = Base - (float)Y;
  if (Variation > 0.0F) {
    Density += Variation * SampleTerrainNoise3D(X, Y, Z, Seed);
  }
  Density -= CeilingTaper(Y);

  // Caves only remove material: where the terrain is already open they cannot
  // change the result, so they are not evaluated.
  if (Density <= 0.0F) {
    return Density;
  }
  float Caves = CaveCornerDensity(Base, ShellDepth, X, Y, Z, Seed);
  return (Caves < Density) ? Caves : Density;
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
            Shape->Base, Shape->Variation, Shape->ShellDepth,
            Column->X0 + (Ix * DENSITY_CELL_XZ),
            Y0 + (Iy * DENSITY_CELL_Y), Column->Z0 + (Iz * DENSITY_CELL_XZ),
            Seed);
      }
    }
  }
}

// The flood cell around a block, kept between calls of one column scan.
typedef struct {
  bool Valid;
  int X0;
  int Y0;
  int Z0;
  float Corners[DENSITY_CELL_CORNERS];
} FloodProbeCell;

// Same corners, order, and fractions as the generator's InterpolateCubeFlood.
static float ProbeFlood(FloodProbeCell *Cell, int X, int Y, int Z,
                        uint64_t Seed) {
  int X0 = FloorToMultiple(X, CAVE_FLOOD_CELL);
  int Y0 = FloorToMultiple(Y, CAVE_FLOOD_CELL);
  int Z0 = FloorToMultiple(Z, CAVE_FLOOD_CELL);
  if (!Cell->Valid || Cell->X0 != X0 || Cell->Y0 != Y0 || Cell->Z0 != Z0) {
    for (int Iz = 0; Iz < 2; Iz++) {
      for (int Iy = 0; Iy < 2; Iy++) {
        for (int Ix = 0; Ix < 2; Ix++) {
          Cell->Corners[Ix + (2 * Iy) + (4 * Iz)] = CaveFloodLattice(
              X0 + (Ix * CAVE_FLOOD_CELL), Y0 + (Iy * CAVE_FLOOD_CELL),
              Z0 + (Iz * CAVE_FLOOD_CELL), Seed);
        }
      }
    }
    Cell->Valid = true;
    Cell->X0 = X0;
    Cell->Y0 = Y0;
    Cell->Z0 = Z0;
  }
  return TrilinearDensity(Cell->Corners,
                          (float)(X - X0) / (float)CAVE_FLOOD_CELL,
                          (float)(Y - Y0) / (float)CAVE_FLOOD_CELL,
                          (float)(Z - Z0) / (float)CAVE_FLOOD_CELL);
}

// An open block in the flood zone near the flood threshold is barrier rock.
// Surface is computed only once some row of the column can be in the zone.
static float ApplyProbeBarrier(float Density, int X, int Y, int Z, int *Surface,
                               FloodProbeCell *Cell, uint64_t Seed) {
  if (Density > 0.0F || !CaveFloodPossible(Y)) {
    return Density;
  }
  if (*Surface == INT_MIN) {
    *Surface = GetPreliminarySurface(X, Z, Seed);
  }
  if (!CaveFloodZone(Y, *Surface)) {
    return Density;
  }
  float Barrier = CaveBarrierDensity(ProbeFlood(Cell, X, Y, Z, Seed));
  return (Barrier > 0.0F) ? Barrier : Density;
}

float SampleTerrainDensity(int X, int Y, int Z, uint64_t Seed) {
  ProbeColumn Column;
  LoadProbeColumn(X, Z, Seed, &Column);
  int Y0 = FloorToMultiple(Y, DENSITY_CELL_Y);

  float Corners[DENSITY_CELL_CORNERS];
  ProbeCellCorners(&Column, Y0, Seed, Corners);
  float Density = TrilinearDensity(
      Corners, (float)(X - Column.X0) / (float)DENSITY_CELL_XZ,
      (float)(Y - Y0) / (float)DENSITY_CELL_Y,
      (float)(Z - Column.Z0) / (float)DENSITY_CELL_XZ);
  int Surface = INT_MIN;
  FloodProbeCell Cell = {.Valid = false};
  return ApplyProbeBarrier(Density, X, Y, Z, &Surface, &Cell, Seed);
}

CaveFluid SampleTerrainFluid(int X, int Y, int Z, uint64_t Seed) {
  int Surface = GetPreliminarySurface(X, Z, Seed);
  bool Flooded = false;
  if (CaveFloodZone(Y, Surface)) {
    FloodProbeCell Cell = {.Valid = false};
    Flooded =
        ProbeFlood(&Cell, X, Y, Z, Seed) > GetCaveParams()->FloodThreshold;
  }
  return CaveFluidAt(Y, Surface, Flooded);
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
  int Surface = INT_MIN;
  FloodProbeCell Cell = {.Valid = false};
  int Y = Start;
  while (Y >= Floor) {
    int Y0 = FloorToMultiple(Y, DENSITY_CELL_Y);
    float Corners[DENSITY_CELL_CORNERS];
    ProbeCellCorners(&Column, Y0, Seed, Corners);
    int Stop = (Y0 > Floor) ? Y0 : Floor;
    for (int Block = Y; Block >= Stop; Block--) {
      float Fy = (float)(Block - Y0) / (float)DENSITY_CELL_Y;
      float Density = ApplyProbeBarrier(TrilinearDensity(Corners, Fx, Fy, Fz),
                                        X, Block, Z, &Surface, &Cell, Seed);
      if (Density > 0.0F) {
        return Block;
      }
    }
    Y = Y0 - 1;
  }
  return Floor - 1;
}
