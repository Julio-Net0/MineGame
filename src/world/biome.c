#include "world/biome.h"
#include "world/block_system.h"
#include "world/prefab.h"
#include "world/terrain_shape.h"
#include "third_party/cJSON.h"
#include "core/noise.h"
#include "core/fileio.h"
#include "core/log.h"
#include "core/utils.h"
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  MAX_FILES_LIMIT = 1024,
  RANGE_FIELD_COUNT = 2,
  COLOR_FIELD_COUNT = 3,
  COLOR_CHANNEL_MAX = 255,
  DEFAULT_WATER_TINT_R = 63,
  DEFAULT_WATER_TINT_G = 118,
  DEFAULT_WATER_TINT_B = 228
};

#define NOISE_MIDPOINT 0.5F
#define UNIT_MIN 0.0F
#define UNIT_MAX 1.0F

// Sentinel band for a range the definition leaves out: wide enough to contain
// any climate value, so an omitted axis simply does not constrain the biome.
#define RANGE_UNBOUNDED_MIN (-1.0e9F)
#define RANGE_UNBOUNDED_MAX (1.0e9F)

#define DEFAULT_SUBSURFACE_DEPTH 3
#define MIN_SUBSURFACE_DEPTH 1
#define MAX_SUBSURFACE_DEPTH 8

// JSON keys of the climate axes, in ClimateAxis order.
static const char *const CLIMATE_AXIS_KEYS[CLIMATE_AXIS_COUNT] = {
    "temperature", "humidity",  "continentalness",
    "erosion",     "weirdness", "peaksValleys"};

static void DefineClimateFields(void);

static BiomeType *GetBiomeRegistry(void) {
  static BiomeType SBiomeRegistry[BIOME_REGISTRY_SIZE];
  return SBiomeRegistry;
}

static int *GetLoadedBiomesCountPtr(void) {
  static int SLoadedBiomesCount = 0;
  return &SLoadedBiomesCount;
}

static NoiseFieldDesc DefaultClimateDesc(float Frequency) {
  const int OCTAVES = 3;
  NoiseFieldDesc Desc = NoiseDefaultDesc();
  Desc.Octaves = OCTAVES;
  Desc.Frequency = Frequency;
  return Desc;
}

static BiomeParams *GetParamsPtr(void) {
  static BiomeParams SParams;
  static bool SInitialized = false;
  if (!SInitialized) {
    const float TEMPERATURE_FREQUENCY = 0.0012F;
    const float HUMIDITY_FREQUENCY = 0.0016F;
    const float CONTINENTALNESS_WEIGHT = 2.0F;
    const float LAPSE = 0.0025F;
    const float LAPSE_START = 64.0F;
    const float STEEP_SLOPE = 1.4F;
    SParams.Temperature = DefaultClimateDesc(TEMPERATURE_FREQUENCY);
    SParams.Humidity = DefaultClimateDesc(HUMIDITY_FREQUENCY);
    for (int Axis = 0; Axis < CLIMATE_AXIS_COUNT; Axis++) {
      SParams.AxisWeights[Axis] = UNIT_MAX;
    }
    SParams.AxisWeights[CLIMATE_CONTINENTALNESS] = CONTINENTALNESS_WEIGHT;
    SParams.Lapse = LAPSE;
    SParams.LapseStart = LAPSE_START;
    SParams.SteepSlope = STEEP_SLOPE;
    SInitialized = true;
  }
  return &SParams;
}

const BiomeParams *GetBiomeParams(void) { return GetParamsPtr(); }

int GetLoadedBiomesCount(void) { return *GetLoadedBiomesCountPtr(); }

// Unloaded slots are included on purpose: GetBiomeDef can fall back to one, and
// its default subsurface depth is then the one generation uses.
int GetMaxBiomeSubsurfaceDepth(void) {
  const BiomeType *Registry = GetBiomeRegistry();
  int MaxDepth = 0;
  for (int Idx = 0; Idx < BIOME_REGISTRY_SIZE; Idx++) {
    if (Registry[Idx].SubsurfaceDepth > MaxDepth) {
      MaxDepth = Registry[Idx].SubsurfaceDepth;
    }
  }
  return MaxDepth;
}

// The surface depth band (see BiomeInSurfaceBand), recomputed after loading.
typedef struct {
  int Low;
  int High;
} DepthBand;

static DepthBand *GetSurfaceBand(void) {
  static DepthBand SBand = {INT_MIN, INT_MAX};
  return &SBand;
}

void InitBiomeRegistry(void) {
  BiomeType *Registry = GetBiomeRegistry();
  for (int Idx = 0; Idx < BIOME_REGISTRY_SIZE; Idx++) {
    Registry[Idx].Id = -1;
    Registry[Idx].Name[0] = '\0';
    Registry[Idx].Family[0] = '\0';
    for (int Axis = 0; Axis < CLIMATE_AXIS_COUNT; Axis++) {
      Registry[Idx].Ranges[Axis] =
          (BiomeRange){RANGE_UNBOUNDED_MIN, RANGE_UNBOUNDED_MAX};
    }
    Registry[Idx].Depth = (BiomeRange){RANGE_UNBOUNDED_MIN, RANGE_UNBOUNDED_MAX};
    // White so an unloaded slot tints nothing rather than turning faces black.
    Registry[Idx].GrassTint = COLOR_WHITE;
    Registry[Idx].FoliageTint = COLOR_WHITE;
    Registry[Idx].WaterTint =
        (Color8){DEFAULT_WATER_TINT_R, DEFAULT_WATER_TINT_G,
                 DEFAULT_WATER_TINT_B, (unsigned char)COLOR_CHANNEL_MAX};
    Registry[Idx].PreviewColor = (Color8){0, 0, 0, 0};
    Registry[Idx].SurfaceBlockId = 0;
    Registry[Idx].SubsurfaceBlockId = 0;
    Registry[Idx].UnderwaterBlockId = 0;
    Registry[Idx].SubsurfaceDepth = DEFAULT_SUBSURFACE_DEPTH;
    Registry[Idx].SteepBlockId = -1;
    Registry[Idx].FrozenWater = false;
    Registry[Idx].FloraCount = 0;
    Registry[Idx].FloraTotalWeight = 0;
    Registry[Idx].FloraDensity = 0.0F;
    Registry[Idx].StructureCount = 0;
    Registry[Idx].StructureTotalWeight = 0;
    Registry[Idx].StructureDensity = 0.0F;
  }
  *GetLoadedBiomesCountPtr() = 0;
  *GetSurfaceBand() = (DepthBand){INT_MIN, INT_MAX};

  // Defaults first, so climate sampling is defined even when no params file
  // loads; LoadBiomeParams redefines the same fields in place.
  DefineClimateFields();
}

const BiomeType *GetBiomeDef(int BiomeId) {
  BiomeType *Registry = GetBiomeRegistry();
  if (BiomeId < 0 || BiomeId >= BIOME_REGISTRY_SIZE) {
    return &Registry[BIOME_FALLBACK_ID];
  }
  if (Registry[BiomeId].Id == -1) {
    return &Registry[BIOME_FALLBACK_ID];
  }
  return &Registry[BiomeId];
}

// ---------------------------------------------------------------------------
// Climate sampling
// ---------------------------------------------------------------------------

// Each axis is its own salted field, so temperature, humidity, and the terrain
// fields never sample correlated noise.
#define TEMPERATURE_FIELD_SALT 0xC11A7E0000000001ULL
#define HUMIDITY_FIELD_SALT 0xC11A7E0000000002ULL

typedef struct {
  NoiseField Temperature;
  NoiseField Humidity;
} ClimateFields;

static ClimateFields *GetClimateFields(void) {
  static ClimateFields SFields = {.Temperature = NOISE_FIELD_INVALID,
                                  .Humidity = NOISE_FIELD_INVALID};
  return &SFields;
}

static void DefineClimateFields(void) {
  const BiomeParams *Params = GetParamsPtr();
  ClimateFields *Climate = GetClimateFields();
  if (!NoiseFieldDefine(&Climate->Temperature, &Params->Temperature,
                        TEMPERATURE_FIELD_SALT) ||
      !NoiseFieldDefine(&Climate->Humidity, &Params->Humidity,
                        HUMIDITY_FIELD_SALT)) {
    LogError("BIOME: noise field table is full; a climate axis is flat");
  }
}

static float ClampUnit(float Value) {
  if (Value < UNIT_MIN) {
    return UNIT_MIN;
  }
  if (Value > UNIT_MAX) {
    return UNIT_MAX;
  }
  return Value;
}

static float SampleUnitField(NoiseField Field, int X, int Z, uint64_t Seed) {
  float Raw = NoiseSample2D(Field, Seed, (float)X, (float)Z);
  return (Raw + UNIT_MAX) * NOISE_MIDPOINT;
}

int BiomeCellCenter(int Coord) {
  int Cell = Coord / BIOME_CELL_SIZE;
  if ((Coord % BIOME_CELL_SIZE != 0) && (Coord < 0)) {
    Cell--;
  }
  return (Cell * BIOME_CELL_SIZE) + (BIOME_CELL_SIZE / 2);
}

BiomeClimate SampleColumnClimate(int X, int Z, int Surface, uint64_t Seed) {
  const BiomeParams *Params = GetParamsPtr();
  const ClimateFields *Fields = GetClimateFields();
  ShapeCoords Coords = SampleShapeCoords(X, Z, Seed);

  float Altitude = fmaxf(0.0F, (float)Surface - Params->LapseStart);
  float Temperature = SampleUnitField(Fields->Temperature, X, Z, Seed) -
                      (Params->Lapse * Altitude);

  BiomeClimate Climate;
  Climate.Axes[CLIMATE_TEMPERATURE] = ClampUnit(Temperature);
  Climate.Axes[CLIMATE_HUMIDITY] =
      ClampUnit(SampleUnitField(Fields->Humidity, X, Z, Seed));
  Climate.Axes[CLIMATE_CONTINENTALNESS] =
      Coords.Values[SHAPE_COORD_CONTINENTALNESS];
  Climate.Axes[CLIMATE_EROSION] = Coords.Values[SHAPE_COORD_EROSION];
  Climate.Axes[CLIMATE_WEIRDNESS] = Coords.Values[SHAPE_COORD_WEIRDNESS];
  Climate.Axes[CLIMATE_PEAKS_VALLEYS] =
      Coords.Values[SHAPE_COORD_PEAKS_VALLEYS];
  Climate.Depth = 0.0F;
  return Climate;
}

static bool RangeContains(BiomeRange Range, float Value) {
  return Value >= Range.Min && Value <= Range.Max;
}

// Distance outside the range, 0 inside it.
static float RangeGap(BiomeRange Range, float Value) {
  if (Value < Range.Min) {
    return Range.Min - Value;
  }
  if (Value > Range.Max) {
    return Value - Range.Max;
  }
  return 0.0F;
}

int ResolveBiome(const BiomeClimate *Climate) {
  const BiomeType *Registry = GetBiomeRegistry();
  const float *Weights = GetParamsPtr()->AxisWeights;
  int BestId = BIOME_FALLBACK_ID;
  float BestDistance = 0.0F;
  bool Found = false;

  // Ascending ids with a strict comparison: equal distances keep the lower id.
  for (int Idx = 0; Idx < BIOME_REGISTRY_SIZE; Idx++) {
    const BiomeType *Biome = &Registry[Idx];
    if (Biome->Id == -1 || !RangeContains(Biome->Depth, Climate->Depth)) {
      continue;
    }
    float Distance = 0.0F;
    for (int Axis = 0; Axis < CLIMATE_AXIS_COUNT; Axis++) {
      float Gap = RangeGap(Biome->Ranges[Axis], Climate->Axes[Axis]);
      Distance += Weights[Axis] * Gap * Gap;
    }
    if (!Found || Distance < BestDistance) {
      BestId = Biome->Id;
      BestDistance = Distance;
      Found = true;
    }
  }
  return BestId;
}

bool BiomeInSurfaceBand(int Depth) {
  const DepthBand *Band = GetSurfaceBand();
  return Depth >= Band->Low && Depth <= Band->High;
}

// The band of integer depths around 0 where no biome's depth range starts or
// ends: inside it, depth-eligibility equals eligibility at depth 0.
static void ComputeSurfaceBand(void) {
  const BiomeType *Registry = GetBiomeRegistry();
  DepthBand Band = {INT_MIN, INT_MAX};
  for (int Idx = 0; Idx < BIOME_REGISTRY_SIZE; Idx++) {
    const BiomeType *Biome = &Registry[Idx];
    if (Biome->Id == -1) {
      continue;
    }
    float Min = Biome->Depth.Min;
    float Max = Biome->Depth.Max;
    if (RangeContains(Biome->Depth, 0.0F)) {
      if (Min > (float)Band.Low) {
        Band.Low = (int)ceilf(Min);
      }
      if (Max < (float)Band.High) {
        Band.High = (int)floorf(Max);
      }
    } else if (Max < 0.0F) {
      Band.Low = ((int)floorf(Max) + 1 > Band.Low) ? (int)floorf(Max) + 1
                                                   : Band.Low;
    } else {
      Band.High = ((int)ceilf(Min) - 1 < Band.High) ? (int)ceilf(Min) - 1
                                                    : Band.High;
    }
  }
  *GetSurfaceBand() = Band;
}

int SampleBiomeAt(int X, int Y, int Z, uint64_t Seed, BiomeClimate *OutClimate) {
  int CenterX = BiomeCellCenter(X);
  int CenterY = BiomeCellCenter(Y);
  int CenterZ = BiomeCellCenter(Z);
  int Surface = GetPreliminarySurface(CenterX, CenterZ, Seed);
  BiomeClimate Climate = SampleColumnClimate(CenterX, CenterZ, Surface, Seed);
  Climate.Depth =
      (float)(GetPreliminaryFloor(CenterX, CenterZ, Seed) - CenterY);
  if (OutClimate != NULL) {
    *OutClimate = Climate;
  }
  return ResolveBiome(&Climate);
}

Color8 GetBiomeTint(int BiomeId, TintSource Source) {
  if (Source == TINT_NONE) {
    return COLOR_WHITE;
  }

  const BiomeType *Biome = GetBiomeDef(BiomeId);
  if (Biome->Id == -1) {
    return COLOR_WHITE;
  }

  switch (Source) {
  case TINT_GRASS:
    return Biome->GrassTint;
  case TINT_FOLIAGE:
    return Biome->FoliageTint;
  case TINT_WATER:
    return Biome->WaterTint;
  case TINT_NONE:
  default:
    return COLOR_WHITE;
  }
}

// ---------------------------------------------------------------------------
// JSON loading
// ---------------------------------------------------------------------------

// Optional [Min, Max] array. An absent or malformed range leaves Out at the
// caller's default rather than failing the biome: an unconstrained axis is a
// meaningful thing for a definition to want.
static void ParseRange(const cJSON *Json, const char *Key, BiomeRange *Out) {
  cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, Key);
  if (cJSON_IsArray(Item) == 0 ||
      cJSON_GetArraySize(Item) != RANGE_FIELD_COUNT) {
    return;
  }

  cJSON *MinItem = cJSON_GetArrayItem(Item, 0);
  cJSON *MaxItem = cJSON_GetArrayItem(Item, 1);
  if (cJSON_IsNumber(MinItem) == 0 || cJSON_IsNumber(MaxItem) == 0) {
    return;
  }

  Out->Min = (float)MinItem->valuedouble;
  Out->Max = (float)MaxItem->valuedouble;
}

// Optional [R, G, B] array. An absent or malformed color leaves Out at white,
// which multiplies to no tint at all.
static void ParseTint(const cJSON *Json, const char *Key, Color8 *Out) {
  cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, Key);
  if (cJSON_IsArray(Item) == 0 ||
      cJSON_GetArraySize(Item) != COLOR_FIELD_COUNT) {
    return;
  }

  unsigned char Channels[COLOR_FIELD_COUNT];
  for (int Idx = 0; Idx < COLOR_FIELD_COUNT; Idx++) {
    cJSON *Channel = cJSON_GetArrayItem(Item, Idx);
    if (cJSON_IsNumber(Channel) == 0) {
      return;
    }
    int Value = Channel->valueint;
    if (Value < 0) {
      Value = 0;
    }
    if (Value > COLOR_CHANNEL_MAX) {
      Value = COLOR_CHANNEL_MAX;
    }
    Channels[Idx] = (unsigned char)Value;
  }

  Out->R = Channels[0];
  Out->G = Channels[1];
  Out->B = Channels[2];
  Out->A = (unsigned char)COLOR_CHANNEL_MAX;
}

// Resolve a palette block name to its registry id at load time, so sampling and
// generation never compare strings. Returns -1 and reports when the name is
// missing or matches no loaded block.
static int ParseBlockRef(const cJSON *Json, const char *Key,
                         const char *FilePath) {
  cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, Key);
  if (cJSON_IsString(Item) == 0) {
    LogError("BIOME: %s missing a valid '%s'", FilePath, Key);
    return -1;
  }

  int BlockId = GetBlockIdByName(Item->valuestring);
  if (BlockId < 0) {
    LogError("BIOME: %s '%s' names '%s', which is not a loaded block", FilePath,
             Key, Item->valuestring);
    return -1;
  }

  return BlockId;
}

// Optional weighted flora set: "flora": [{ "block": "TallGrass", "weight": N }]
// plus "floraDensity" in [0, 1]. Unresolved block names and non-positive weights
// are skipped; an absent set leaves the biome bare.
static void ParseFlora(const cJSON *Json, const char *FilePath,
                       BiomeType *Biome) {
  Biome->FloraCount = 0;
  Biome->FloraTotalWeight = 0;
  Biome->FloraDensity = 0.0F;

  cJSON *DensityItem = cJSON_GetObjectItemCaseSensitive(Json, "floraDensity");
  if (cJSON_IsNumber(DensityItem) != 0) {
    float Density = (float)DensityItem->valuedouble;
    if (Density < UNIT_MIN) {
      Density = UNIT_MIN;
    }
    if (Density > UNIT_MAX) {
      Density = UNIT_MAX;
    }
    Biome->FloraDensity = Density;
  }

  cJSON *FloraItem = cJSON_GetObjectItemCaseSensitive(Json, "flora");
  if (cJSON_IsArray(FloraItem) == 0) {
    return;
  }

  int Count = cJSON_GetArraySize(FloraItem);
  for (int Idx = 0; Idx < Count && Biome->FloraCount < MAX_BIOME_FLORA; Idx++) {
    cJSON *Entry = cJSON_GetArrayItem(FloraItem, Idx);
    cJSON *BlockItem = cJSON_GetObjectItemCaseSensitive(Entry, "block");
    cJSON *WeightItem = cJSON_GetObjectItemCaseSensitive(Entry, "weight");
    if (cJSON_IsString(BlockItem) == 0) {
      continue;
    }

    int BlockId = GetBlockIdByName(BlockItem->valuestring);
    if (BlockId < 0) {
      LogWarn("BIOME: %s flora '%s' is not a loaded block, skipping", FilePath,
              BlockItem->valuestring);
      continue;
    }

    int Weight = (cJSON_IsNumber(WeightItem) != 0) ? WeightItem->valueint : 1;
    if (Weight <= 0) {
      continue;
    }

    Biome->Flora[Biome->FloraCount].BlockId = BlockId;
    Biome->Flora[Biome->FloraCount].Weight = Weight;
    Biome->FloraCount++;
    Biome->FloraTotalWeight += Weight;
  }
}

// Optional weighted structure set: "structures": [{ "prefab": "oak_small",
// "weight": N }] plus "structureDensity" in [0, 1]. Prefab names resolve to
// registry indices at load; unknown names and non-positive weights are skipped;
// an absent set leaves the biome bare.
static void ParseStructures(const cJSON *Json, const char *FilePath,
                           BiomeType *Biome) {
  Biome->StructureCount = 0;
  Biome->StructureTotalWeight = 0;
  Biome->StructureDensity = 0.0F;

  cJSON *DensityItem =
      cJSON_GetObjectItemCaseSensitive(Json, "structureDensity");
  if (cJSON_IsNumber(DensityItem) != 0) {
    float Density = (float)DensityItem->valuedouble;
    if (Density < UNIT_MIN) {
      Density = UNIT_MIN;
    }
    if (Density > UNIT_MAX) {
      Density = UNIT_MAX;
    }
    Biome->StructureDensity = Density;
  }

  cJSON *StructuresItem = cJSON_GetObjectItemCaseSensitive(Json, "structures");
  if (cJSON_IsArray(StructuresItem) == 0) {
    return;
  }

  int Count = cJSON_GetArraySize(StructuresItem);
  for (int Idx = 0;
       Idx < Count && Biome->StructureCount < MAX_BIOME_STRUCTURES; Idx++) {
    cJSON *Entry = cJSON_GetArrayItem(StructuresItem, Idx);
    cJSON *PrefabItem = cJSON_GetObjectItemCaseSensitive(Entry, "prefab");
    cJSON *WeightItem = cJSON_GetObjectItemCaseSensitive(Entry, "weight");
    if (cJSON_IsString(PrefabItem) == 0) {
      continue;
    }

    int PrefabIndex = GetPrefabIndexByName(PrefabItem->valuestring);
    if (PrefabIndex < 0) {
      LogWarn("BIOME: %s structure '%s' is not a loaded prefab, skipping",
              FilePath, PrefabItem->valuestring);
      continue;
    }

    int Weight = (cJSON_IsNumber(WeightItem) != 0) ? WeightItem->valueint : 1;
    if (Weight <= 0) {
      continue;
    }

    Biome->Structures[Biome->StructureCount].PrefabIndex = PrefabIndex;
    Biome->Structures[Biome->StructureCount].Weight = Weight;
    Biome->StructureCount++;
    Biome->StructureTotalWeight += Weight;
  }
}

static void ParseBiomeFile(const char *FilePath) {
  char *FileContent = ReadTextFile(FilePath);
  if (FileContent == NULL) {
    LogError("BIOME: failed to read file: %s", FilePath);
    return;
  }

  cJSON *Json = cJSON_Parse(FileContent);
  if (Json == NULL) {
    LogError("BIOME: JSON syntax error in %s before: %s", FilePath,
             cJSON_GetErrorPtr());
    free(FileContent);
    return;
  }

  cJSON *IdItem = cJSON_GetObjectItemCaseSensitive(Json, "id");
  if (cJSON_IsNumber(IdItem) == 0) {
    LogError("BIOME: %s does not have a valid 'id'", FilePath);
    cJSON_Delete(Json);
    free(FileContent);
    return;
  }

  int ParsedId = IdItem->valueint;
  if (ParsedId < 0 || ParsedId >= BIOME_REGISTRY_SIZE) {
    LogError("BIOME: id %d from %s is out of the allowed range (0-%d)",
             ParsedId, FilePath, BIOME_REGISTRY_SIZE - 1);
    cJSON_Delete(Json);
    free(FileContent);
    return;
  }

  int SurfaceId = ParseBlockRef(Json, "surfaceBlock", FilePath);
  int SubsurfaceId = ParseBlockRef(Json, "subsurfaceBlock", FilePath);
  int UnderwaterId = ParseBlockRef(Json, "underwaterBlock", FilePath);
  if (SurfaceId < 0 || SubsurfaceId < 0 || UnderwaterId < 0) {
    LogError("BIOME: skipping %s, its palette does not resolve", FilePath);
    cJSON_Delete(Json);
    free(FileContent);
    return;
  }

  BiomeType *Registry = GetBiomeRegistry();
  BiomeType *Biome = &Registry[ParsedId];
  bool IsNewEntry = (Biome->Id == -1);
  Biome->Id = ParsedId;
  Biome->SurfaceBlockId = SurfaceId;
  Biome->SubsurfaceBlockId = SubsurfaceId;
  Biome->UnderwaterBlockId = UnderwaterId;

  cJSON *NameItem = cJSON_GetObjectItemCaseSensitive(Json, "name");
  if (cJSON_IsString(NameItem) != 0) {
    SafeStrncpy(Biome->Name, NameItem->valuestring, MAX_BIOME_NAME_SIZE);
  }
  cJSON *FamilyItem = cJSON_GetObjectItemCaseSensitive(Json, "family");
  if (cJSON_IsString(FamilyItem) != 0) {
    SafeStrncpy(Biome->Family, FamilyItem->valuestring, MAX_BIOME_FAMILY_SIZE);
  }

  for (int Axis = 0; Axis < CLIMATE_AXIS_COUNT; Axis++) {
    ParseRange(Json, CLIMATE_AXIS_KEYS[Axis], &Biome->Ranges[Axis]);
  }
  ParseRange(Json, "depth", &Biome->Depth);

  ParseTint(Json, "grassTint", &Biome->GrassTint);
  ParseTint(Json, "foliageTint", &Biome->FoliageTint);
  ParseTint(Json, "waterTint", &Biome->WaterTint);
  ParseTint(Json, "previewColor", &Biome->PreviewColor);

  cJSON *SubsurfaceDepthItem =
      cJSON_GetObjectItemCaseSensitive(Json, "subsurfaceDepth");
  if (cJSON_IsNumber(SubsurfaceDepthItem) != 0) {
    Biome->SubsurfaceDepth = SubsurfaceDepthItem->valueint;
  }
  // Terrain generation sees at most 8 blocks above a cube to decide palette
  // layers, so a deeper layer could not be rendered consistently.
  if (Biome->SubsurfaceDepth < MIN_SUBSURFACE_DEPTH ||
      Biome->SubsurfaceDepth > MAX_SUBSURFACE_DEPTH) {
    int Clamped = (Biome->SubsurfaceDepth < MIN_SUBSURFACE_DEPTH)
                      ? MIN_SUBSURFACE_DEPTH
                      : MAX_SUBSURFACE_DEPTH;
    LogWarn("BIOME: %s subsurfaceDepth %d is outside %d..%d, using %d",
            FilePath, Biome->SubsurfaceDepth, MIN_SUBSURFACE_DEPTH,
            MAX_SUBSURFACE_DEPTH, Clamped);
    Biome->SubsurfaceDepth = Clamped;
  }

  if (cJSON_GetObjectItemCaseSensitive(Json, "priority") != NULL) {
    LogWarn("BIOME: %s declares 'priority', which is ignored: the nearest "
            "climate ranges decide",
            FilePath);
  }

  Biome->SteepBlockId = -1;
  if (cJSON_GetObjectItemCaseSensitive(Json, "steepBlock") != NULL) {
    Biome->SteepBlockId = ParseBlockRef(Json, "steepBlock", FilePath);
  }
  Biome->FrozenWater =
      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(Json, "frozenWater")) != 0;

  ParseFlora(Json, FilePath, Biome);
  ParseStructures(Json, FilePath, Biome);

  if (IsNewEntry) {
    (*GetLoadedBiomesCountPtr())++;
  }

  LogInfo("BIOME: Loaded [%d] %s (depth %.0f..%.0f)", ParsedId, Biome->Name,
          (double)Biome->Depth.Min, (double)Biome->Depth.Max);

  cJSON_Delete(Json);
  free(FileContent);
}

void LoadBiomeParams(const char *FilePath) {
  char *FileContent = ReadTextFile(FilePath);
  if (FileContent == NULL) {
    LogWarn("BIOME: no params at %s, keeping built-in noise defaults",
            FilePath);
    return;
  }

  cJSON *Json = cJSON_Parse(FileContent);
  if (Json == NULL) {
    LogError("BIOME: JSON syntax error in %s before: %s, keeping defaults",
             FilePath, cJSON_GetErrorPtr());
    free(FileContent);
    return;
  }

  BiomeParams *Params = GetParamsPtr();
  const cJSON *Temperature =
      cJSON_GetObjectItemCaseSensitive(Json, "temperatureNoise");
  if (cJSON_IsObject(Temperature) != 0) {
    ReadNoiseFieldDesc(Temperature, &Params->Temperature, FilePath);
  }
  const cJSON *Humidity = cJSON_GetObjectItemCaseSensitive(Json, "humidityNoise");
  if (cJSON_IsObject(Humidity) != 0) {
    ReadNoiseFieldDesc(Humidity, &Params->Humidity, FilePath);
  }

  const cJSON *Weights = cJSON_GetObjectItemCaseSensitive(Json, "axisWeights");
  for (int Axis = 0; Axis < CLIMATE_AXIS_COUNT; Axis++) {
    const cJSON *Weight =
        cJSON_GetObjectItemCaseSensitive(Weights, CLIMATE_AXIS_KEYS[Axis]);
    if (cJSON_IsNumber(Weight) != 0 && Weight->valuedouble >= 0.0) {
      Params->AxisWeights[Axis] = (float)Weight->valuedouble;
    }
  }

  const struct {
    const char *Key;
    float *Out;
  } SCALARS[] = {{"lapse", &Params->Lapse},
                 {"lapseStart", &Params->LapseStart},
                 {"steepSlope", &Params->SteepSlope}};
  for (size_t Idx = 0; Idx < sizeof(SCALARS) / sizeof(SCALARS[0]); Idx++) {
    const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, SCALARS[Idx].Key);
    if (cJSON_IsNumber(Item) != 0) {
      *SCALARS[Idx].Out = (float)Item->valuedouble;
    }
  }

  LogInfo("BIOME: climate T freq %.4f, H freq %.4f, lapse %.4f from Y %.0f, "
          "steep slope %.2f",
          (double)Params->Temperature.Frequency,
          (double)Params->Humidity.Frequency, (double)Params->Lapse,
          (double)Params->LapseStart, (double)Params->SteepSlope);

  DefineClimateFields();

  cJSON_Delete(Json);
  free(FileContent);
}

void LoadAllBiomeDefinitions(const char *DirectoryPath) {
  char Path[FILEIO_MAX_PATH];
  snprintf(Path, sizeof(Path), "./%s", DirectoryPath);

  LogInfo("BIOME: Attempting to load from: %s", Path);

  static char FilePaths[MAX_FILES_LIMIT][FILEIO_MAX_PATH];
  int FilesCount = ListDirectoryFiles(Path, FilePaths, MAX_FILES_LIMIT);
  if (FilesCount == 0) {
    LogWarn("BIOME: Directory not found or empty %s", Path);
    return;
  }

  for (int Idx = 0; Idx < FilesCount; Idx++) {
    if (HasFileExtension(FilePaths[Idx], ".json")) {
      ParseBiomeFile(FilePaths[Idx]);
    }
  }

  ComputeSurfaceBand();
  LogInfo("BIOME: surface depth band %d..%d resolves once per cell column",
          GetSurfaceBand()->Low, GetSurfaceBand()->High);
  int LoadedCount = GetLoadedBiomesCount();
  if (LoadedCount == 0) {
    LogWarn("BIOME: No valid biome loaded from %s", Path);
  } else {
    LogInfo("BIOME: Loaded %d biome definitions", LoadedCount);
  }
}
