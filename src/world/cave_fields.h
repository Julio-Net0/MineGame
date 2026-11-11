#ifndef WORLD_CAVE_FIELDS_H
#define WORLD_CAVE_FIELDS_H

#include <stdbool.h>
#include <stdint.h>

// Noise caves carved out of the overworld density. Every term is in blocks and
// is evaluated only at density grid corners, so caves interpolate with the
// terrain and the density probe stays bit-identical to generation:
//
//   d      = (dT <= 0) ? dT : min(dT, Caves)
//   Caves  = inside the surface shell ? shell term (> 0, no cave sampled)
//          : below the cave floor     ? +inf (no cave sampled)
//          : min(cheese, spaghetti, noodle) + floor fade
//
// Spaghetti tunnels are horizontal: the world is cut into bands of
// SpaghettiLayerSpacing blocks, and each band holds one sheet of tunnels whose
// plan view is the zero line of a 2D path field and whose height follows a 2D
// elevation field. Every field is shifted per band, so stacked bands differ.
// Noodles stay 3D (their thin shafts connect the bands).
//
// Flooded caves: below sea level and under the ocean band, an open position is
// water where the flood field exceeds FloodThreshold and air where it does not.
// The flood field is sampled on its own 16-block lattice, independent of the
// cave short-circuits, and interpolated per block. Wherever the interpolated
// value lies within BarrierBand of the threshold the block is rock: the field
// changes by far less than twice the band from one block to the next, so every
// boundary between flooded and dry cave space is sealed by rock.
//
// Tuning lives in assets/worldgen/caves.json and the fields in
// assets/worldgen/noise.json. Written during startup only; read-only and
// lock-free afterwards.

enum {
  CAVE_MAX_THRESHOLDS = 16,
  CAVE_FLOOD_CELL = 16
};

typedef enum {
  CAVE_FLUID_AIR = 0,
  CAVE_FLUID_WATER,
  CAVE_FLUID_LAVA
} CaveFluid;

typedef struct {
  bool CheeseEnabled;
  bool SpaghettiEnabled;
  bool NoodleEnabled;
  int ThresholdCount;
  float ThresholdY[CAVE_MAX_THRESHOLDS];
  float ThresholdValue[CAVE_MAX_THRESHOLDS];
  float SpaghettiWidth;
  float SpaghettiWidthVariation;
  int SpaghettiLayerSpacing;
  // Half the tunnel height in blocks. Must exceed half a grid cell, or a tunnel
  // whose centre falls between two corner rows is not opened at all.
  float SpaghettiHalfHeight;
  float SpaghettiElevationAmplitude;
  float NoodleWidth;
  float NoodleToggle;
  // Blocks of density per unit of cave noise.
  float CaveScale;
  float ShellBase;
  // Shell where ocean or lake water can stand above the cave space (sea floors
  // and low coasts). Thicker than ShellBase by at least one 8-block grid cell,
  // because interpolation lets a cave rise up to a cell into the shell, and it
  // must never reach the water band.
  float ShellUnderwater;
  float EntranceDepth;
  int CaveFloor;
  float FloorFade;
  int LavaLevel;
  float FloodThreshold;
  float BarrierBand;
} CaveParams;

void InitCaveFields(void);

// Cave noise fields from noise.json; missing entries keep their defaults.
void LoadCaveNoise(const char *FilePath);

// caves.json; a rejected file falls back to the built-in parameters with the
// reason logged.
void LoadCaveParams(const char *FilePath);

const CaveParams *GetCaveParams(void);

bool CavesEnabled(void);

// Shell depth of a lattice column: how far below Base no cave may open.
float CaveShellDepth(int LatticeX, int LatticeZ, float Base, uint64_t Seed);

// The Caves term at a grid corner, given the column's Base and shell depth.
// Returns FLT_MAX where no cave can exist.
float CaveCornerDensity(float Base, float ShellDepth, int X, int Y, int Z,
                        uint64_t Seed);

// The flood field at a point of its lattice (every coordinate a multiple of
// CAVE_FLOOD_CELL).
float CaveFloodLattice(int X, int Y, int Z, uint64_t Seed);

// Whether Y can lie in the flood zone of any column: caves enabled, at or below
// sea level, above lava. Cheap; callers test it before computing Surface.
bool CaveFloodPossible(int Y);

// Whether an open position can hold cave water: CaveFloodPossible and below the
// ocean band of its column (Surface is the preliminary surface).
bool CaveFloodZone(int Y, int Surface);

// The flood zone of one column as a range of Y, exactly the Y for which
// CaveFloodZone holds. Returns false when the range is empty.
bool CaveFloodRows(int Surface, int *Lowest, int *Highest);

// Density of the barrier for an interpolated flood value: positive (solid)
// within BarrierBand of FloodThreshold, non-positive elsewhere.
float CaveBarrierDensity(float Flood);

// Fluid of a non-solid overworld position below the ceiling. Flooded means the
// position is in the flood zone and its interpolated flood value exceeds
// FloodThreshold.
CaveFluid CaveFluidAt(int Y, int Surface, bool Flooded);

#endif
