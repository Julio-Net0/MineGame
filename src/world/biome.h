#ifndef BIOME_H
#define BIOME_H

#include "core/color.h"
#include "core/tint.h"
#include <stdint.h>

// Renderer-free, engine-free biome sampling.
//
// This header deliberately includes no chunk, world, or render header, and the
// sampling functions below are pure: they depend only on their arguments and on
// the biome registry, which is read-only once loaded. Compiling biome.c against
// only core/noise, cJSON, core/fileio, and the block registry is therefore
// enough to sample the exact biome function the game uses, which is what lets an
// external biome visualizer/editor reuse it without linking the engine.
//
// Climate has three axes. Temperature and Humidity come from independent 2D fBm
// noise fields over (X, Z), normalized to [0,1]. Depth is TerrainHeight - Y and
// costs no noise sample at all — it is what makes biome selection a function of
// Y as well as X and Z, so surface and cave biomes can share a column.
//
// Noise tuning lives in data, not here: LoadBiomeParams reads scale, octaves,
// lacunarity, and gain from assets/biome_params.json, and biome definitions come
// from assets/biomes/*.json. Nothing in this module is compiled-in tuning.

enum {
  BIOME_REGISTRY_SIZE = 64,
  MAX_BIOME_NAME_SIZE = 32,
  // Biome resolution: one biome id per BIOME_CELL_SIZE^3 block cell. Climate is
  // a low-frequency field, so storing one biome per block would cost 64x the
  // memory to record a value that barely changes across a cell.
  BIOME_CELL_SIZE = 4,
  // Returned when no biome matches a climate point.
  BIOME_FALLBACK_ID = 0
};

// Inclusive [Min, Max] band on one climate axis.
typedef struct {
  float Min;
  float Max;
} BiomeRange;

typedef struct {
  float Temperature;
  float Humidity;
  float Depth;
} BiomeClimate;

// A biome definition with palette block names already resolved to registry ids
// at load time, so sampling never compares strings.
typedef struct {
  BiomeRange Temperature;
  BiomeRange Humidity;
  BiomeRange Depth;
  Color8 GrassTint;
  Color8 FoliageTint;
  int Id;
  int SurfaceBlockId;
  int SubsurfaceBlockId;
  int UnderwaterBlockId;
  int SubsurfaceDepth;
  // Highest priority wins when a climate point falls inside several biomes.
  int Priority;
  char Name[MAX_BIOME_NAME_SIZE];
} BiomeType;

typedef struct {
  float Scale;
  float Lacunarity;
  float Gain;
  int Octaves;
} BiomeNoiseParams;

void InitBiomeRegistry(void);

// Load the shared climate-noise tuning. Missing or malformed files leave the
// built-in defaults in place rather than failing.
void LoadBiomeParams(const char *FilePath);

// Parse and register every .json in DirectoryPath. Must run after the block
// registry is loaded (palette names resolve against it) and before the chunk
// workers start (the registry must not be written while they sample it).
void LoadAllBiomeDefinitions(const char *DirectoryPath);

const BiomeType *GetBiomeDef(int BiomeId);
const BiomeNoiseParams *GetBiomeNoiseParams(void);
int GetLoadedBiomesCount(void);

BiomeClimate SampleBiomeClimate(int X, int Y, int Z, int TerrainHeight,
                                uint64_t Seed);
int ResolveBiome(BiomeClimate Climate);
int SampleBiomeAt(int X, int Y, int Z, int TerrainHeight, uint64_t Seed);

// Biome tint for a face declaring Source. White for TINT_NONE or an unloaded
// biome, so an untinted face multiplies by 1 and keeps its current color.
Color8 GetBiomeTint(int BiomeId, TintSource Source);

#endif
