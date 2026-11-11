#include "world/surface.h"
#include "world/chunk.h"
#include "world/terrain_density.h"
#include "world/terrain_shape.h"
#include <math.h>
#include <stdbool.h>

enum {
  SPAWN_SEARCH_RADIUS = 256,
  SPAWN_DROP_HEIGHT = 3
};

static int FloorToLattice(int Value) {
  int Quotient = Value / SHAPE_LATTICE_STEP;
  if ((Value % SHAPE_LATTICE_STEP != 0) && (Value < 0)) {
    Quotient--;
  }
  return Quotient * SHAPE_LATTICE_STEP;
}

int GetPreliminarySurface(int GlobalX, int GlobalZ, uint64_t Seed) {
  int X0 = FloorToLattice(GlobalX);
  int Z0 = FloorToLattice(GlobalZ);
  float Fx = (float)(GlobalX - X0) / (float)SHAPE_LATTICE_STEP;
  float Fz = (float)(GlobalZ - Z0) / (float)SHAPE_LATTICE_STEP;

  float B00 = EvaluateColumnShape(X0, Z0, Seed).Base;
  float B10 = EvaluateColumnShape(X0 + SHAPE_LATTICE_STEP, Z0, Seed).Base;
  float B01 = EvaluateColumnShape(X0, Z0 + SHAPE_LATTICE_STEP, Seed).Base;
  float B11 = EvaluateColumnShape(X0 + SHAPE_LATTICE_STEP,
                                  Z0 + SHAPE_LATTICE_STEP, Seed)
                  .Base;
  return (int)floorf(ShapeBilinear(B00, B10, B01, B11, Fx, Fz));
}

// A column qualifies when its terrain top is above sea level. The preliminary
// surface is checked first: it is far cheaper, and a column whose 2D estimate
// lies under the sea is skipped without probing the 3D field.
static bool ColumnQualifies(int X, int Z, uint64_t Seed, int *OutTop) {
  if (GetPreliminarySurface(X, Z, Seed) <= SEA_LEVEL) {
    return false;
  }
  int Top = GetTerrainTop(X, Z, Seed);
  if (Top <= SEA_LEVEL) {
    return false;
  }
  *OutTop = Top;
  return true;
}

// Walks ring R of the square spiral in a fixed order (top edge, right, bottom,
// left) and reports the first qualifying column.
static bool SearchRing(int Ring, uint64_t Seed, SpawnPoint *Out) {
  int Side = 2 * Ring;
  int PosX = -Ring;
  int PosZ = -Ring;
  const int STEP_X[4] = {1, 0, -1, 0};
  const int STEP_Z[4] = {0, 1, 0, -1};

  for (int Edge = 0; Edge < 4; Edge++) {
    for (int Step = 0; Step < Side; Step++) {
      int Top = 0;
      if (ColumnQualifies(PosX, PosZ, Seed, &Top)) {
        Out->X = PosX;
        Out->Z = PosZ;
        Out->Surface = Top;
        return true;
      }
      PosX += STEP_X[Edge];
      PosZ += STEP_Z[Edge];
    }
  }
  return false;
}

static SpawnPoint SearchSpawnColumn(uint64_t Seed) {
  SpawnPoint Spawn = {.X = 0, .Z = 0, .Surface = 0};

  bool Found = ColumnQualifies(0, 0, Seed, &Spawn.Surface);
  for (int Ring = 1; Ring <= SPAWN_SEARCH_RADIUS && !Found; Ring++) {
    Found = SearchRing(Ring, Seed, &Spawn);
  }
  if (!Found) {
    Spawn.X = 0;
    Spawn.Z = 0;
    Spawn.Surface = GetTerrainTop(0, 0, Seed);
  }

  int Ground = (Spawn.Surface > SEA_LEVEL) ? Spawn.Surface : SEA_LEVEL;
  Spawn.FeetPosition = (Vec3){(float)Spawn.X,
                              (float)(Ground + SPAWN_DROP_HEIGHT),
                              (float)Spawn.Z};
  return Spawn;
}

// Startup, the server, and every script state dump ask for the spawn; the search
// is deterministic, so it runs once per seed. Only the main thread calls this.
SpawnPoint FindSpawnColumn(uint64_t Seed) {
  static bool SHasCached = false;
  static uint64_t SCachedSeed = 0;
  static SpawnPoint SCached;

  if (!SHasCached || SCachedSeed != Seed) {
    SCached = SearchSpawnColumn(Seed);
    SCachedSeed = Seed;
    SHasCached = true;
  }
  return SCached;
}
