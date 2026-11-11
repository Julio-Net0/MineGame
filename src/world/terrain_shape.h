#ifndef WORLD_TERRAIN_SHAPE_H
#define WORLD_TERRAIN_SHAPE_H

#include <stdbool.h>
#include <stdint.h>

// Large-scale terrain shape: four 2D fields (continentalness, erosion,
// weirdness, and peaks-and-valleys derived from weirdness) feed nested cubic
// Hermite splines that give, per lattice column, the terrain's base height and
// how far 3D noise may displace it. Curves live in
// assets/worldgen/terrain_shape.json and noise parameters in
// assets/worldgen/noise.json; nothing here is compiled-in tuning beyond the
// fallback used when a file is missing or rejected.
//
// Shape is evaluated only at lattice columns (X and Z multiples of
// SHAPE_LATTICE_STEP); everything between is bilinear. Written during startup,
// before the chunk workers exist; read-only and lock-free afterwards.

enum {
  SHAPE_LATTICE_STEP = 4
};

// Upper bound on |3D terrain noise| used to bound density without sampling it.
// The library normalizes fractal output to about [-1, 1]; the margin keeps the
// bound safe, and the worldgen self-test checks it on its samples.
#define TERRAIN_NOISE_BOUND 1.1F

typedef enum {
  SHAPE_COORD_CONTINENTALNESS = 0,
  SHAPE_COORD_EROSION,
  SHAPE_COORD_WEIRDNESS,
  SHAPE_COORD_PEAKS_VALLEYS,
  SHAPE_COORD_COUNT
} ShapeCoordinate;

typedef struct {
  float Values[SHAPE_COORD_COUNT];
} ShapeCoords;

typedef struct {
  // Base height with jaggedness applied, in blocks.
  float Base;
  // How far, in blocks, 3D noise may push the surface up or down.
  float Variation;
} ColumnShape;

// Installs the built-in fields and shape. Must run before the loaders.
void InitTerrainShape(void);

// Shape and 3D terrain noise fields from noise.json; missing entries keep their
// defaults.
void LoadTerrainNoise(const char *FilePath);

// Splines and ceiling taper from terrain_shape.json; a rejected file falls back
// to the built-in shape with the reason logged.
void LoadTerrainShape(const char *FilePath);

ShapeCoords SampleShapeCoords(int GlobalX, int GlobalZ, uint64_t Seed);

// The one shape function: the column cache, the preliminary surface, and the
// density probe all call it, so they cannot diverge.
ColumnShape EvaluateColumnShape(int LatticeX, int LatticeZ, uint64_t Seed);

float GetCeilingTaper(void);

// The 3D terrain field n3, with its vertical scale applied to Y.
float SampleTerrainNoise3D(int X, int Y, int Z, uint64_t Seed);

// Verifies every spline reproduces each numeric control point at its location
// and is continuous there. On failure writes a description and returns false.
bool TerrainShapeSelfCheck(char *Failure, int FailureSize, int *OutChecked);

// Bilinear blend of the four lattice-corner values around a block column. One
// definition shared by the pure preliminary surface and the column cache, so
// both produce bit-identical floats.
static inline float ShapeBilinear(float V00, float V10, float V01, float V11,
                                  float Fx, float Fz) {
  float Low = V00 + ((V10 - V00) * Fx);
  float High = V01 + ((V11 - V01) * Fx);
  return Low + ((High - Low) * Fz);
}

#endif
