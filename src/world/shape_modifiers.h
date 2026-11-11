#ifndef WORLD_SHAPE_MODIFIERS_H
#define WORLD_SHAPE_MODIFIERS_H

#include <stdbool.h>
#include <stdint.h>

// Biome landforms as 2D adjustments of a lattice column's Base and Variation,
// applied inside EvaluateColumnShape after the splines, so every consumer of the
// column shape (density, probe, cache, bounds, preliminary surface) sees them.
//
// Each modifier is weighted by the climate: 1 inside its axis ranges, falling
// linearly to 0 across each axis's fade margin, so a landform blends into its
// neighbours instead of ending in a wall at a biome border. Tuning lives in
// assets/worldgen/shape_modifiers.json; written during startup only.

enum {
  SHAPE_MODIFIER_MAX = 16,
  // Weights are evaluated on this coarser lattice and blended bilinearly, so a
  // landform fades in over at least this many blocks however fast the climate
  // changes; a 4-block lattice step can change a weight by at most 0.25.
  SHAPE_MODIFIER_WEIGHT_CELL = 16,
  SHAPE_MODIFIER_AXES = 6,
  SHAPE_MODIFIER_MAX_BIOME_NAME = 32
};

// Axis order: temperature, humidity, continentalness, erosion, weirdness,
// peaks-and-valleys (the biome climate's order).
typedef enum {
  MODIFIER_AXIS_TEMPERATURE = 0,
  MODIFIER_AXIS_HUMIDITY,
  MODIFIER_AXIS_CONTINENTALNESS,
  MODIFIER_AXIS_EROSION,
  MODIFIER_AXIS_WEIRDNESS,
  MODIFIER_AXIS_PEAKS_VALLEYS
} ModifierAxisIndex;

typedef enum {
  SHAPE_MODIFIER_TERRACES = 0,
  SHAPE_MODIFIER_SPIRES,
  SHAPE_MODIFIER_SHATTERED,
  SHAPE_MODIFIER_DUNES
} ShapeModifierType;

typedef struct {
  bool Declared;
  float Min;
  float Max;
  float Fade;
} ModifierAxis;

typedef struct {
  ShapeModifierType Type;
  ModifierAxis Axes[SHAPE_MODIFIER_AXES];
  // Type parameters; unused ones stay 0.
  float Lift;
  float Height;
  float Step;
  float Power;
  float Damp;
  float Amplify;
  float Wavelength;
  float Direction;
  float Warp;
  float Windward;
  char Biome[SHAPE_MODIFIER_MAX_BIOME_NAME];
  // Registry id of Biome, resolved at load; -1 when none is named.
  int BiomeId;
  // With a biome: the selection margin over which the weight rises from 0 at
  // the biome's border to 1 inside it.
  float Margin;
} ShapeModifier;

void InitShapeModifiers(void);

// The modifiers' own fields ("spire", "duneWarp") from noise.json.
void LoadShapeModifierNoise(const char *FilePath);

// shape_modifiers.json; a missing or rejected file leaves no modifier active.
void LoadShapeModifiers(const char *FilePath);

int GetShapeModifierCount(void);
const ShapeModifier *GetShapeModifier(int Index);

// 1 inside [Min, Max], linear to 0 across Fade outside it, 0 beyond.
float ModifierAxisRamp(const ModifierAxis *Axis, float Value);

// Axis-range part of Modifier's weight for a climate (axes in
// ModifierAxisIndex order); the biome margin part is not included.
float ShapeModifierWeightOf(const ShapeModifier *Modifier,
                            const float Climate[SHAPE_MODIFIER_AXES]);

// Biome part of the weight: 0 at or outside the biome's border, rising
// linearly to 1 once its selection margin reaches Margin.
float ModifierMarginRamp(const ShapeModifier *Modifier, float SelectionMargin);

// Weight of loaded modifier Index at column (X, Z): the bilinear blend of its
// weights at the four surrounding weight-lattice points, exactly what the
// column shape uses.
float ShapeModifierWeightAt(int Index, int X, int Z, uint64_t Seed);

// Applies every loaded modifier, in file order, to one lattice column.
void ApplyShapeModifiers(int X, int Z, float *Base, float *Variation,
                         uint64_t Seed);

#endif
