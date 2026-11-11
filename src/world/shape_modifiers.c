#include "world/shape_modifiers.h"
#include "core/fileio.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/noise.h"
#include "core/utils.h"
#include "third_party/cJSON.h"
#include "world/biome.h"
#include "world/climate.h"
#include "world/terrain_shape.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  MODIFIER_FIELD_SPIRE = 0,
  MODIFIER_FIELD_DUNE_WARP,
  MODIFIER_FIELD_COUNT
} ModifierField;

typedef struct {
  ShapeModifier Modifiers[SHAPE_MODIFIER_MAX];
  int Count;
  NoiseFieldDesc Descs[MODIFIER_FIELD_COUNT];
  NoiseField Fields[MODIFIER_FIELD_COUNT];
} ModifierState;

static const char *const AXIS_KEYS[SHAPE_MODIFIER_AXES] = {
    "temperature", "humidity",  "continentalness",
    "erosion",     "weirdness", "peaksValleys"};

static const char *const FIELD_KEYS[MODIFIER_FIELD_COUNT] = {"spire",
                                                             "duneWarp"};

static const uint64_t FIELD_SALTS[MODIFIER_FIELD_COUNT] = {
    0x5A9E000000000001ULL, 0x5A9E000000000002ULL};

static const char *const TYPE_NAMES[] = {"terraces", "spires", "shattered",
                                         "dunes"};

static ModifierState *GetModifierState(void) {
  static ModifierState SState;
  return &SState;
}

static void DefineFields(ModifierState *State) {
  for (int Field = 0; Field < MODIFIER_FIELD_COUNT; Field++) {
    if (!NoiseFieldDefine(&State->Fields[Field], &State->Descs[Field],
                          FIELD_SALTS[Field])) {
      LogError("SHAPE: noise field table is full; field '%s' samples as 0",
               FIELD_KEYS[Field]);
    }
  }
}

void InitShapeModifiers(void) {
  const float SPIRE_FREQUENCY = 0.03F;
  const float WARP_FREQUENCY = 0.01F;
  ModifierState *State = GetModifierState();
  State->Count = 0;
  for (int Field = 0; Field < MODIFIER_FIELD_COUNT; Field++) {
    State->Fields[Field] = NOISE_FIELD_INVALID;
    State->Descs[Field] = NoiseDefaultDesc();
    State->Descs[Field].Octaves = 1;
    State->Descs[Field].Fractal = NOISE_FRACTAL_NONE;
  }
  State->Descs[MODIFIER_FIELD_SPIRE].Frequency = SPIRE_FREQUENCY;
  State->Descs[MODIFIER_FIELD_DUNE_WARP].Frequency = WARP_FREQUENCY;
  DefineFields(State);
}

void LoadShapeModifierNoise(const char *FilePath) {
  ModifierState *State = GetModifierState();
  char *Content = ReadTextFile(FilePath);
  cJSON *Json = (Content != NULL) ? cJSON_Parse(Content) : NULL;
  free(Content);
  if (Json == NULL) {
    LogWarn("SHAPE: could not read %s, keeping default modifier fields",
            FilePath);
    return;
  }
  for (int Field = 0; Field < MODIFIER_FIELD_COUNT; Field++) {
    const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, FIELD_KEYS[Field]);
    if (cJSON_IsObject(Item) != 0) {
      ReadNoiseFieldDesc(Item, &State->Descs[Field], FilePath);
    }
  }
  cJSON_Delete(Json);
  DefineFields(State);
}

static float ReadNumber(const cJSON *Json, const char *Key, float Fallback) {
  const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, Key);
  return (cJSON_IsNumber(Item) != 0) ? (float)Item->valuedouble : Fallback;
}

// [min, max, fade]; fade defaults to 0 (a hard edge).
static bool ParseAxis(const cJSON *Item, ModifierAxis *Out) {
  enum { RANGE_FIELDS = 2, WITH_FADE = 3 };
  int Size = (cJSON_IsArray(Item) != 0) ? cJSON_GetArraySize(Item) : 0;
  if (Size != RANGE_FIELDS && Size != WITH_FADE) {
    return false;
  }
  Out->Declared = true;
  Out->Min = (float)cJSON_GetArrayItem(Item, 0)->valuedouble;
  Out->Max = (float)cJSON_GetArrayItem(Item, 1)->valuedouble;
  Out->Fade = (Size == WITH_FADE)
                  ? (float)cJSON_GetArrayItem(Item, RANGE_FIELDS)->valuedouble
                  : 0.0F;
  return Out->Max >= Out->Min && Out->Fade >= 0.0F;
}

static bool ParseType(const cJSON *Json, ShapeModifierType *Out) {
  const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, "type");
  if (cJSON_IsString(Item) == 0) {
    return false;
  }
  for (int Type = 0; Type < (int)(sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]));
       Type++) {
    if (strcmp(Item->valuestring, TYPE_NAMES[Type]) == 0) {
      *Out = (ShapeModifierType)Type;
      return true;
    }
  }
  return false;
}

static bool ParseModifier(const cJSON *Json, ShapeModifier *Out) {
  const float DEFAULT_WINDWARD = 0.75F;
  memset(Out, 0, sizeof(*Out));
  if (!ParseType(Json, &Out->Type)) {
    return false;
  }
  for (int Axis = 0; Axis < SHAPE_MODIFIER_AXES; Axis++) {
    const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, AXIS_KEYS[Axis]);
    if (Item != NULL && !ParseAxis(Item, &Out->Axes[Axis])) {
      return false;
    }
  }
  Out->Lift = ReadNumber(Json, "lift", 0.0F);
  Out->Height = ReadNumber(Json, "height", 0.0F);
  Out->Step = ReadNumber(Json, "step", 0.0F);
  Out->Power = ReadNumber(Json, "power", 1.0F);
  Out->Damp = ReadNumber(Json, "damp", 0.0F);
  Out->Amplify = ReadNumber(Json, "amplify", 0.0F);
  Out->Wavelength = ReadNumber(Json, "wavelength", 0.0F);
  Out->Direction = ReadNumber(Json, "direction", 0.0F);
  Out->Warp = ReadNumber(Json, "warp", 0.0F);
  Out->Windward = ReadNumber(Json, "windward", DEFAULT_WINDWARD);
  Out->Margin = ReadNumber(Json, "margin", 0.0F);
  Out->BiomeId = -1;
  const cJSON *Biome = cJSON_GetObjectItemCaseSensitive(Json, "biome");
  if (cJSON_IsString(Biome) != 0) {
    SafeStrncpy(Out->Biome, Biome->valuestring, SHAPE_MODIFIER_MAX_BIOME_NAME);
    Out->BiomeId = FindBiomeByName(Out->Biome);
    if (Out->BiomeId < 0 || Out->Margin <= 0.0F) {
      LogError("SHAPE: modifier biome '%s' is not loaded or its margin is not "
               "positive",
               Out->Biome);
      return false;
    }
  }

  bool StepOk = Out->Type != SHAPE_MODIFIER_TERRACES || Out->Step > 0.0F;
  bool WaveOk = Out->Type != SHAPE_MODIFIER_DUNES ||
                (Out->Wavelength > 0.0F && Out->Windward > 0.0F &&
                 Out->Windward < 1.0F);
  return StepOk && WaveOk && Out->Power > 0.0F && Out->Damp >= 0.0F &&
         Out->Damp <= 1.0F;
}

void LoadShapeModifiers(const char *FilePath) {
  ModifierState *State = GetModifierState();
  State->Count = 0;
  char *Content = ReadTextFile(FilePath);
  cJSON *Json = (Content != NULL) ? cJSON_Parse(Content) : NULL;
  free(Content);
  const cJSON *List = cJSON_GetObjectItemCaseSensitive(Json, "modifiers");
  if (cJSON_IsArray(List) == 0) {
    LogWarn("SHAPE: no modifier list in %s, no landform modifiers active",
            FilePath);
    cJSON_Delete(Json);
    return;
  }

  ShapeModifier Parsed[SHAPE_MODIFIER_MAX];
  int Count = cJSON_GetArraySize(List);
  bool Ok = Count <= SHAPE_MODIFIER_MAX;
  for (int Idx = 0; Ok && Idx < Count; Idx++) {
    Ok = ParseModifier(cJSON_GetArrayItem(List, Idx), &Parsed[Idx]);
    if (!Ok) {
      LogError("SHAPE: %s modifier %d has an unknown type, a bad range, or a "
               "parameter out of range",
               FilePath, Idx);
    }
  }
  if (Ok) {
    memcpy(State->Modifiers, Parsed, sizeof(ShapeModifier) * (size_t)Count);
    State->Count = Count;
    LogInfo("SHAPE: %d landform modifiers", Count);
  } else {
    LogError("SHAPE: %s rejected, no landform modifiers active", FilePath);
  }
  cJSON_Delete(Json);
}

int GetShapeModifierCount(void) { return GetModifierState()->Count; }

const ShapeModifier *GetShapeModifier(int Index) {
  const ModifierState *State = GetModifierState();
  return (Index >= 0 && Index < State->Count) ? &State->Modifiers[Index]
                                              : NULL;
}

// ---------------------------------------------------------------------------
// Weights
// ---------------------------------------------------------------------------

float ModifierAxisRamp(const ModifierAxis *Axis, float Value) {
  if (!Axis->Declared || (Value >= Axis->Min && Value <= Axis->Max)) {
    return 1.0F;
  }
  if (Axis->Fade <= 0.0F) {
    return 0.0F;
  }
  float Gap = (Value < Axis->Min) ? Axis->Min - Value : Value - Axis->Max;
  return fmaxf(0.0F, 1.0F - (Gap / Axis->Fade));
}

float ShapeModifierWeightOf(const ShapeModifier *Modifier,
                            const float Climate[SHAPE_MODIFIER_AXES]) {
  float Weight = 1.0F;
  for (int Axis = 0; Axis < SHAPE_MODIFIER_AXES; Axis++) {
    Weight *= ModifierAxisRamp(&Modifier->Axes[Axis], Climate[Axis]);
  }
  return Weight;
}

float ModifierMarginRamp(const ShapeModifier *Modifier, float SelectionMargin) {
  return fminf(1.0F, fmaxf(0.0F, SelectionMargin / Modifier->Margin));
}

// Climate of one lattice column, sampled on first need and shared by every
// modifier evaluated there.
typedef struct {
  bool Sampled;
  BiomeClimate Climate;
} LatticeClimate;

static const BiomeClimate *GetLatticeClimate(LatticeClimate *Cache, int X,
                                             int Z, float Base,
                                             const float Coords[4],
                                             uint64_t Seed) {
  if (!Cache->Sampled) {
    BiomeClimate *Climate = &Cache->Climate;
    Climate->Axes[CLIMATE_TEMPERATURE] = ClimateTemperature(X, Z, Base, Seed);
    Climate->Axes[CLIMATE_HUMIDITY] = ClimateHumidity(X, Z, Seed);
    Climate->Axes[CLIMATE_CONTINENTALNESS] = Coords[0];
    Climate->Axes[CLIMATE_EROSION] = Coords[1];
    Climate->Axes[CLIMATE_WEIRDNESS] = Coords[2];
    Climate->Axes[CLIMATE_PEAKS_VALLEYS] = Coords[3];
    Climate->Depth = 0.0F;
    Cache->Sampled = true;
  }
  return &Cache->Climate;
}

// The shape axes first; the climate is sampled only when those leave a
// non-zero weight and the modifier needs temperature, humidity, or its biome.
static float WeightAt(const ShapeModifier *Modifier, int X, int Z, float Base,
                      const float Coords[4], uint64_t Seed,
                      LatticeClimate *Cache) {
  float Weight = 1.0F;
  for (int Shape = 0; Shape < 4 && Weight > 0.0F; Shape++) {
    Weight *= ModifierAxisRamp(
        &Modifier->Axes[MODIFIER_AXIS_CONTINENTALNESS + Shape], Coords[Shape]);
  }
  bool NeedsClimate = Modifier->Axes[MODIFIER_AXIS_TEMPERATURE].Declared ||
                      Modifier->Axes[MODIFIER_AXIS_HUMIDITY].Declared ||
                      Modifier->BiomeId >= 0;
  if (Weight <= 0.0F || !NeedsClimate) {
    return Weight;
  }
  const BiomeClimate *Climate =
      GetLatticeClimate(Cache, X, Z, Base, Coords, Seed);
  Weight *= ModifierAxisRamp(&Modifier->Axes[MODIFIER_AXIS_TEMPERATURE],
                             Climate->Axes[CLIMATE_TEMPERATURE]);
  Weight *= ModifierAxisRamp(&Modifier->Axes[MODIFIER_AXIS_HUMIDITY],
                             Climate->Axes[CLIMATE_HUMIDITY]);
  if (Weight > 0.0F && Modifier->BiomeId >= 0) {
    Weight *= ModifierMarginRamp(
        Modifier, BiomeSelectionMargin(Modifier->BiomeId, Climate));
  }
  return Weight;
}

// Every modifier's weight at one weight-lattice point, from that point's own
// unmodified shape and climate. Memoized per thread: each point serves the 16
// lattice columns around it.
typedef struct {
  int X;
  int Z;
  uint64_t Seed;
  bool Valid;
  float Weights[SHAPE_MODIFIER_MAX];
} WeightMemoEntry;

enum {
  WEIGHT_MEMO_SLOTS = 256
};

static const float *PointWeights(int X, int Z, uint64_t Seed) {
  static _Thread_local WeightMemoEntry SMemo[WEIGHT_MEMO_SLOTS];
  const unsigned int HALF_BITS = 32U;
  uint64_t Key = ((uint64_t)(uint32_t)X << HALF_BITS) | (uint64_t)(uint32_t)Z;
  WeightMemoEntry *Entry =
      &SMemo[SplitMix64(Key ^ Seed) % (uint64_t)WEIGHT_MEMO_SLOTS];
  if (Entry->Valid && Entry->X == X && Entry->Z == Z && Entry->Seed == Seed) {
    return Entry->Weights;
  }
  const ModifierState *State = GetModifierState();
  ColumnShape Raw = EvaluateColumnShapeUnmodified(X, Z, Seed);
  ShapeCoords Coords = SampleShapeCoords(X, Z, Seed);
  LatticeClimate Cache = {.Sampled = false};
  for (int Idx = 0; Idx < State->Count; Idx++) {
    Entry->Weights[Idx] = WeightAt(&State->Modifiers[Idx], X, Z, Raw.Base,
                                   Coords.Values, Seed, &Cache);
  }
  Entry->X = X;
  Entry->Z = Z;
  Entry->Seed = Seed;
  Entry->Valid = true;
  return Entry->Weights;
}

static int FloorToWeightCell(int Value) {
  int Cell = Value / SHAPE_MODIFIER_WEIGHT_CELL;
  if ((Value % SHAPE_MODIFIER_WEIGHT_CELL != 0) && (Value < 0)) {
    Cell--;
  }
  return Cell * SHAPE_MODIFIER_WEIGHT_CELL;
}

// Bilinear blend of every modifier's weight at (X, Z).
static void BlendWeights(int X, int Z, uint64_t Seed,
                         float Out[SHAPE_MODIFIER_MAX]) {
  const ModifierState *State = GetModifierState();
  int X0 = FloorToWeightCell(X);
  int Z0 = FloorToWeightCell(Z);
  float Fx = (float)(X - X0) / (float)SHAPE_MODIFIER_WEIGHT_CELL;
  float Fz = (float)(Z - Z0) / (float)SHAPE_MODIFIER_WEIGHT_CELL;
  // Copied out: the four lookups may share memo slots.
  float W00[SHAPE_MODIFIER_MAX];
  float W10[SHAPE_MODIFIER_MAX];
  float W01[SHAPE_MODIFIER_MAX];
  float W11[SHAPE_MODIFIER_MAX];
  size_t Bytes = sizeof(float) * (size_t)State->Count;
  memcpy(W00, PointWeights(X0, Z0, Seed), Bytes);
  memcpy(W10, PointWeights(X0 + SHAPE_MODIFIER_WEIGHT_CELL, Z0, Seed), Bytes);
  memcpy(W01, PointWeights(X0, Z0 + SHAPE_MODIFIER_WEIGHT_CELL, Seed), Bytes);
  memcpy(W11,
         PointWeights(X0 + SHAPE_MODIFIER_WEIGHT_CELL,
                      Z0 + SHAPE_MODIFIER_WEIGHT_CELL, Seed),
         Bytes);
  for (int Idx = 0; Idx < State->Count; Idx++) {
    float Low = W00[Idx] + ((W10[Idx] - W00[Idx]) * Fx);
    float High = W01[Idx] + ((W11[Idx] - W01[Idx]) * Fx);
    Out[Idx] = Low + ((High - Low) * Fz);
  }
}

float ShapeModifierWeightAt(int Index, int X, int Z, uint64_t Seed) {
  if (Index < 0 || Index >= GetModifierState()->Count) {
    return 0.0F;
  }
  float Weights[SHAPE_MODIFIER_MAX];
  BlendWeights(X, Z, Seed, Weights);
  return Weights[Index];
}

// ---------------------------------------------------------------------------
// Landforms
// ---------------------------------------------------------------------------

static float SampleField(ModifierField Field, float X, float Z, uint64_t Seed) {
  return NoiseSample2D(GetModifierState()->Fields[Field], Seed, X, Z);
}

// Asymmetric ridge over one wavelength: a long rise over the windward share,
// then a short steep drop.
static float DuneRidge(float Phase, float Windward) {
  return (Phase < Windward) ? Phase / Windward
                            : (1.0F - Phase) / (1.0F - Windward);
}

static void ApplyOne(const ShapeModifier *Modifier, float Weight, int X, int Z,
                     float *Base, float *Variation, uint64_t Seed) {
  const float DEGREES_TO_RADIANS = 0.017453292F;
  switch (Modifier->Type) {
  case SHAPE_MODIFIER_TERRACES: {
    float Lifted = *Base + (Modifier->Lift * Weight);
    float Steps = Lifted / Modifier->Step;
    float Floor = floorf(Steps);
    float Stepped = (Modifier->Step * Floor) +
                    (Modifier->Step * powf(Steps - Floor, Modifier->Power));
    *Base = Lifted + ((Stepped - Lifted) * Weight);
    *Variation *= 1.0F - (Modifier->Damp * Weight);
    break;
  }
  case SHAPE_MODIFIER_SPIRES: {
    float Spire = SampleField(MODIFIER_FIELD_SPIRE, (float)X, (float)Z, Seed);
    *Base += Weight * Modifier->Height * powf(fmaxf(0.0F, Spire), Modifier->Power);
    *Variation *= 1.0F - (Modifier->Damp * Weight);
    break;
  }
  case SHAPE_MODIFIER_SHATTERED:
    *Base += Weight * Modifier->Lift;
    *Variation *= 1.0F + (Modifier->Amplify * Weight);
    break;
  case SHAPE_MODIFIER_DUNES: {
    float Angle = Modifier->Direction * DEGREES_TO_RADIANS;
    float Warp = SampleField(MODIFIER_FIELD_DUNE_WARP, (float)X, (float)Z, Seed);
    float Along = ((float)X * cosf(Angle)) + ((float)Z * sinf(Angle)) +
                  (Modifier->Warp * Warp);
    float Waves = Along / Modifier->Wavelength;
    float Phase = Waves - floorf(Waves);
    *Base += Weight * Modifier->Height * DuneRidge(Phase, Modifier->Windward);
    *Variation *= 1.0F - (Modifier->Damp * Weight);
    break;
  }
  default:
    break;
  }
}

void ApplyShapeModifiers(int X, int Z, float *Base, float *Variation,
                         uint64_t Seed) {
  const ModifierState *State = GetModifierState();
  if (State->Count == 0) {
    return;
  }
  // Weights come from the weight lattice's unmodified shapes, so a landform
  // never moves its own or a later modifier's climate.
  float Weights[SHAPE_MODIFIER_MAX];
  BlendWeights(X, Z, Seed, Weights);
  for (int Idx = 0; Idx < State->Count; Idx++) {
    if (Weights[Idx] > 0.0F) {
      ApplyOne(&State->Modifiers[Idx], Weights[Idx], X, Z, Base, Variation,
               Seed);
    }
  }
  *Variation = fmaxf(0.0F, *Variation);
}
