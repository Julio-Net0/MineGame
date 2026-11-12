#ifndef BIOME_H
#define BIOME_H

#include "core/color.h"
#include "core/noise.h"
#include "core/tint.h"
#include <stdbool.h>
#include <stdint.h>

// Renderer-free, engine-free biome sampling.
//
// This header deliberately includes no chunk, world, or render header, and the
// sampling functions below are pure: they depend only on their arguments, the
// biome registry, and the terrain shape fields, all read-only once loaded.
// Compiling biome.c against core/noise, cJSON, core/fileio, the block registry,
// and world/terrain_shape is therefore enough to sample the exact biome
// function the game uses.
//
// A biome cell is described at its centre column by six climate axes plus
// depth. Temperature and humidity are salted 2D fBm fields mapped to [0,1],
// temperature lowered with altitude; continentalness, erosion, weirdness, and
// peaks-and-valleys are the terrain shape's own coordinates. Depth is
// PreliminaryFloor - Y: below the lowest level terrain noise can open, so a
// positive depth beyond the surface band is cave space, never a cliff face.
//
// Selection filters biomes by depth range, then picks the one whose declared
// axis ranges lie nearest the cell's climate (weighted squared gap). Tuning
// lives in assets/biome_params.json and assets/biomes/*.json.

enum {
  BIOME_REGISTRY_SIZE = 64,
  MAX_BIOME_NAME_SIZE = 32,
  MAX_BIOME_FAMILY_SIZE = 16,
  // Upper bound on distinct flora kinds a single biome can scatter.
  MAX_BIOME_FLORA = 8,
  // Upper bound on distinct prefab structures a single biome can place.
  MAX_BIOME_STRUCTURES = 8,
  // Biome resolution: one biome id per BIOME_CELL_SIZE^3 block cell. Climate is
  // a low-frequency field, so storing one biome per block would cost 64x the
  // memory to record a value that barely changes across a cell.
  BIOME_CELL_SIZE = 4,
  // Returned when no biome matches a climate point.
  BIOME_FALLBACK_ID = 0
};

typedef enum {
  CLIMATE_TEMPERATURE = 0,
  CLIMATE_HUMIDITY,
  CLIMATE_CONTINENTALNESS,
  CLIMATE_EROSION,
  CLIMATE_WEIRDNESS,
  CLIMATE_PEAKS_VALLEYS,
  CLIMATE_AXIS_COUNT
} ClimateAxis;

// Inclusive [Min, Max] band on one climate axis.
typedef struct {
  float Min;
  float Max;
} BiomeRange;

typedef struct {
  float Axes[CLIMATE_AXIS_COUNT];
  float Depth;
} BiomeClimate;

// One flora kind a biome can grow, with its relative selection weight. Block
// names are resolved to registry ids at load time so placement never compares
// strings.
typedef struct {
  int BlockId;
  int Weight;
} BiomeFloraEntry;

// One prefab structure a biome can place, with its relative selection weight.
// Prefab names are resolved to registry indices at load time so placement never
// compares strings.
typedef struct {
  int PrefabIndex;
  int Weight;
} BiomeStructureEntry;

// A biome definition with palette block names already resolved to registry ids
// at load time, so sampling never compares strings.
typedef struct {
  // An axis the definition leaves out spans everything and never adds distance.
  BiomeRange Ranges[CLIMATE_AXIS_COUNT];
  BiomeRange Depth;
  Color8 GrassTint;
  Color8 FoliageTint;
  Color8 WaterTint;
  // Pixel color in preview biome maps; alpha 0 when the definition has none.
  Color8 PreviewColor;
  int Id;
  int SurfaceBlockId;
  int SubsurfaceBlockId;
  int UnderwaterBlockId;
  int SubsurfaceDepth;
  // Replaces the surface, underwater, and subsurface blocks where the terrain
  // is steep; -1 when the biome declares none.
  int SteepBlockId;
  // Sea-level ocean water in this biome is ice.
  bool FrozenWater;
  // Optional flora set: weighted kinds scattered on this biome's surface by the
  // feature pass. FloraCount 0 leaves the biome bare. FloraDensity is the
  // fraction of eligible surface columns that receive flora, in [0, 1].
  BiomeFloraEntry Flora[MAX_BIOME_FLORA];
  int FloraCount;
  int FloraTotalWeight;
  float FloraDensity;
  // Optional structure set: weighted prefabs stamped on this biome's surface by
  // the feature pass. StructureCount 0 leaves the biome bare. StructureDensity
  // is the fraction of eligible candidate columns that receive a structure, in
  // [0, 1].
  BiomeStructureEntry Structures[MAX_BIOME_STRUCTURES];
  int StructureCount;
  int StructureTotalWeight;
  float StructureDensity;
  char Name[MAX_BIOME_NAME_SIZE];
  // Optional grouping for tools and coverage checks ("ocean", "coast",
  // "lowland", "highland", "peak", "underground"); empty when undeclared.
  char Family[MAX_BIOME_FAMILY_SIZE];
} BiomeType;

typedef struct {
  NoiseFieldDesc Temperature;
  NoiseFieldDesc Humidity;
  float AxisWeights[CLIMATE_AXIS_COUNT];
  // Temperature drops by Lapse per block of preliminary surface above
  // LapseStart.
  float Lapse;
  float LapseStart;
  // Slope (horizontal over vertical density gradient) above which a biome's
  // steep block replaces its palette layers.
  float SteepSlope;
} BiomeParams;

void InitBiomeRegistry(void);

// Load the shared climate tuning. Missing or malformed files leave the built-in
// defaults in place rather than failing.
void LoadBiomeParams(const char *FilePath);

// Parse and register every .json in DirectoryPath. Must run after the block
// registry is loaded (palette names resolve against it) and before the chunk
// workers start (the registry must not be written while they sample it).
void LoadAllBiomeDefinitions(const char *DirectoryPath);

const BiomeType *GetBiomeDef(int BiomeId);
const BiomeParams *GetBiomeParams(void);
int GetLoadedBiomesCount(void);

// Deepest subsurface layer any biome slot declares. Terrain classification uses
// it to prove a cube lies entirely below every biome's palette layers.
int GetMaxBiomeSubsurfaceDepth(void);

// Centre coordinate of the biome cell containing Coord (any axis).
int BiomeCellCenter(int Coord);

// Climate of the column at (X, Z), whose preliminary surface is Surface; Depth
// is left 0. Callers pass biome cell centre columns.
BiomeClimate SampleColumnClimate(int X, int Z, int Surface, uint64_t Seed);

int ResolveBiome(const BiomeClimate *Climate);

// How decisively BiomeId wins at Climate: the square root of the nearest other
// depth-eligible biome's distance minus the square root of BiomeId's own.
// Positive inside the biome's selected region and continuous there; 0 on its
// border. Outside it the result is only guaranteed to be <= 0: the scan stops at
// the first biome at least as near.
float BiomeSelectionMargin(int BiomeId, const BiomeClimate *Climate);

// Registry id of the loaded biome named Name, or -1.
int FindBiomeByName(const char *Name);

// Depths over which the set of depth-eligible biomes equals the set at depth 0,
// so a surface resolution made at depth 0 holds for the whole band.
bool BiomeInSurfaceBand(int Depth);

// The biome at (X, Y, Z), evaluated at the centre of its biome cell exactly as
// terrain generation fills the biome map. OutClimate may be NULL.
int SampleBiomeAt(int X, int Y, int Z, uint64_t Seed, BiomeClimate *OutClimate);

// Biome tint for a face declaring Source. White for TINT_NONE or an unloaded
// biome, so an untinted face multiplies by 1 and keeps its current color.
Color8 GetBiomeTint(int BiomeId, TintSource Source);

// Locates the nearest cell of BiomeId within MaxRadius blocks from (StartX, StartZ)
// in concentric square rings with Step block increments. Returns true if found,
// filling OutX, OutZ, and OutDistance.
bool FindBiomeLocation(int BiomeId, int StartX, int StartZ, int MaxRadius,
                       uint64_t Seed, int *OutX, int *OutZ, float *OutDistance);

#endif
