#include "world/biome.h"
#include "world/block_system.h"
#include "third_party/cJSON.h"
#include "core/noise.h"
#include "core/fileio.h"
#include "core/log.h"
#include "core/utils.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  MAX_FILES_LIMIT = 1024,
  RANGE_FIELD_COUNT = 2,
  COLOR_FIELD_COUNT = 3,
  COLOR_CHANNEL_MAX = 255,
  // Climate axis indices. Each gets its own hashed noise offsets so the fields
  // stay decorrelated from each other and from the terrain-height field.
  AXIS_TEMPERATURE = 0,
  AXIS_HUMIDITY = 1
};

#define DEFAULT_NOISE_SCALE 0.008F
#define DEFAULT_NOISE_LACUNARITY 2.0F
#define DEFAULT_NOISE_GAIN 0.5F
#define DEFAULT_NOISE_OCTAVES 3

#define NOISE_MIDPOINT 0.5F
#define UNIT_MIN 0.0F
#define UNIT_MAX 1.0F

// Sentinel band for a range the definition leaves out: wide enough to contain
// any climate value, so an omitted axis simply does not constrain the biome.
#define RANGE_UNBOUNDED_MIN (-1.0e9F)
#define RANGE_UNBOUNDED_MAX (1.0e9F)

#define DEFAULT_SUBSURFACE_DEPTH 3

static BiomeType *GetBiomeRegistry(void) {
  static BiomeType SBiomeRegistry[BIOME_REGISTRY_SIZE];
  return SBiomeRegistry;
}

static int *GetLoadedBiomesCountPtr(void) {
  static int SLoadedBiomesCount = 0;
  return &SLoadedBiomesCount;
}

static BiomeNoiseParams *GetNoiseParamsPtr(void) {
  static BiomeNoiseParams SNoiseParams = {
      .Scale = DEFAULT_NOISE_SCALE,
      .Lacunarity = DEFAULT_NOISE_LACUNARITY,
      .Gain = DEFAULT_NOISE_GAIN,
      .Octaves = DEFAULT_NOISE_OCTAVES};
  return &SNoiseParams;
}

const BiomeNoiseParams *GetBiomeNoiseParams(void) { return GetNoiseParamsPtr(); }

int GetLoadedBiomesCount(void) { return *GetLoadedBiomesCountPtr(); }

void InitBiomeRegistry(void) {
  BiomeType *Registry = GetBiomeRegistry();
  #pragma unroll 4
  for (int Idx = 0; Idx < BIOME_REGISTRY_SIZE; Idx++) {
    Registry[Idx].Id = -1;
    Registry[Idx].Name[0] = '\0';
    Registry[Idx].Temperature =
        (BiomeRange){RANGE_UNBOUNDED_MIN, RANGE_UNBOUNDED_MAX};
    Registry[Idx].Humidity =
        (BiomeRange){RANGE_UNBOUNDED_MIN, RANGE_UNBOUNDED_MAX};
    Registry[Idx].Depth = (BiomeRange){RANGE_UNBOUNDED_MIN, RANGE_UNBOUNDED_MAX};
    // White so an unloaded slot tints nothing rather than turning faces black.
    Registry[Idx].GrassTint = COLOR_WHITE;
    Registry[Idx].FoliageTint = COLOR_WHITE;
    Registry[Idx].SurfaceBlockId = 0;
    Registry[Idx].SubsurfaceBlockId = 0;
    Registry[Idx].UnderwaterBlockId = 0;
    Registry[Idx].SubsurfaceDepth = DEFAULT_SUBSURFACE_DEPTH;
    Registry[Idx].Priority = 0;
  }
  *GetLoadedBiomesCountPtr() = 0;
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

// SplitMix64: one multiply-xor-shift round per stage, enough to turn adjacent
// seeds and axis indices into uncorrelated offsets.
static uint64_t SplitMix64(uint64_t Value) {
  const uint64_t GOLDEN_GAMMA = 0x9E3779B97F4A7C15ULL;
  const uint64_t MIX_A = 0xBF58476D1CE4E5B9ULL;
  const uint64_t MIX_B = 0x94D049BB133111EBULL;
  const unsigned int SHIFT_A = 30U;
  const unsigned int SHIFT_B = 27U;
  const unsigned int SHIFT_C = 31U;

  Value += GOLDEN_GAMMA;
  Value = (Value ^ (Value >> SHIFT_A)) * MIX_A;
  Value = (Value ^ (Value >> SHIFT_B)) * MIX_B;
  return Value ^ (Value >> SHIFT_C);
}

static float OffsetFromHash(uint64_t Hash) {
  const uint64_t OFFSET_RANGE = 100000ULL;
  const float OFFSET_HALF = 50000.0F;
  return (float)(Hash % OFFSET_RANGE) - OFFSET_HALF;
}

// Two offsets per axis, hashed from the seed. Sampling each axis at a different
// place in the same noise field is what keeps temperature, humidity, and the
// terrain-height field independent of one another.
static void GetAxisOffsets(uint64_t Seed, int Axis, float *OffX, float *OffZ) {
  const uint64_t AXIS_SPREAD = 0x9E3779B97F4A7C15ULL;
  const uint64_t SALT_X = 0x11ULL;
  const uint64_t SALT_Z = 0x22ULL;

  uint64_t Base = Seed ^ ((uint64_t)Axis * AXIS_SPREAD);
  *OffX = OffsetFromHash(SplitMix64(Base + SALT_X));
  *OffZ = OffsetFromHash(SplitMix64(Base + SALT_Z));
}

// NoiseFbm3 sums octaves without normalizing, so its range grows with octave
// count and gain. Dividing by the amplitude sum brings it back to roughly
// [-1, 1] regardless of how the params are tuned.
static float NoiseAmplitudeSum(const BiomeNoiseParams *Params) {
  float Amplitude = 1.0F;
  float Sum = 0.0F;
  #pragma unroll 4
  for (int Idx = 0; Idx < Params->Octaves; Idx++) {
    Sum += Amplitude;
    Amplitude *= Params->Gain;
  }
  return (Sum > 0.0F) ? Sum : 1.0F;
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

static float SampleClimateAxis(int GlobalX, int GlobalZ, int Axis,
                               uint64_t Seed) {
  const BiomeNoiseParams *Params = GetNoiseParamsPtr();
  float OffX = 0.0F;
  float OffZ = 0.0F;
  GetAxisOffsets(Seed, Axis, &OffX, &OffZ);

  float Raw = NoiseFbm3(((float)GlobalX + OffX) * Params->Scale, 0.0F,
                        ((float)GlobalZ + OffZ) * Params->Scale,
                        Params->Lacunarity, Params->Gain, Params->Octaves);

  float Normalized = ((Raw / NoiseAmplitudeSum(Params)) + UNIT_MAX) * NOISE_MIDPOINT;
  return ClampUnit(Normalized);
}

BiomeClimate SampleBiomeClimate(int X, int Y, int Z, int TerrainHeight,
                                uint64_t Seed) {
  BiomeClimate Climate;
  Climate.Temperature = SampleClimateAxis(X, Z, AXIS_TEMPERATURE, Seed);
  Climate.Humidity = SampleClimateAxis(X, Z, AXIS_HUMIDITY, Seed);
  // Derived, not sampled: distance below the surface of this column.
  Climate.Depth = (float)(TerrainHeight - Y);
  return Climate;
}

static bool RangeContains(BiomeRange Range, float Value) {
  return Value >= Range.Min && Value <= Range.Max;
}

int ResolveBiome(BiomeClimate Climate) {
  const BiomeType *Registry = GetBiomeRegistry();
  int BestId = BIOME_FALLBACK_ID;
  int BestPriority = 0;
  bool Found = false;

  #pragma unroll 4
  for (int Idx = 0; Idx < BIOME_REGISTRY_SIZE; Idx++) {
    const BiomeType *Biome = &Registry[Idx];
    if (Biome->Id == -1) {
      continue;
    }
    if (!RangeContains(Biome->Temperature, Climate.Temperature)) {
      continue;
    }
    if (!RangeContains(Biome->Humidity, Climate.Humidity)) {
      continue;
    }
    if (!RangeContains(Biome->Depth, Climate.Depth)) {
      continue;
    }
    if (!Found || Biome->Priority > BestPriority) {
      BestId = Biome->Id;
      BestPriority = Biome->Priority;
      Found = true;
    }
  }

  return BestId;
}

int SampleBiomeAt(int X, int Y, int Z, int TerrainHeight, uint64_t Seed) {
  return ResolveBiome(SampleBiomeClimate(X, Y, Z, TerrainHeight, Seed));
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
  #pragma unroll 4
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

  ParseRange(Json, "temperature", &Biome->Temperature);
  ParseRange(Json, "humidity", &Biome->Humidity);
  ParseRange(Json, "depth", &Biome->Depth);

  ParseTint(Json, "grassTint", &Biome->GrassTint);
  ParseTint(Json, "foliageTint", &Biome->FoliageTint);

  cJSON *SubsurfaceDepthItem =
      cJSON_GetObjectItemCaseSensitive(Json, "subsurfaceDepth");
  if (cJSON_IsNumber(SubsurfaceDepthItem) != 0) {
    Biome->SubsurfaceDepth = SubsurfaceDepthItem->valueint;
  }

  cJSON *PriorityItem = cJSON_GetObjectItemCaseSensitive(Json, "priority");
  if (cJSON_IsNumber(PriorityItem) != 0) {
    Biome->Priority = PriorityItem->valueint;
  }

  if (IsNewEntry) {
    (*GetLoadedBiomesCountPtr())++;
  }

  LogInfo("BIOME: Loaded [%d] %s (T[%.2f-%.2f] H[%.2f-%.2f] D[%.0f-%.0f])",
          ParsedId, Biome->Name, (double)Biome->Temperature.Min,
          (double)Biome->Temperature.Max, (double)Biome->Humidity.Min,
          (double)Biome->Humidity.Max, (double)Biome->Depth.Min,
          (double)Biome->Depth.Max);

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

  BiomeNoiseParams *Params = GetNoiseParamsPtr();

  cJSON *ScaleItem = cJSON_GetObjectItemCaseSensitive(Json, "scale");
  if (cJSON_IsNumber(ScaleItem) != 0) {
    Params->Scale = (float)ScaleItem->valuedouble;
  }

  cJSON *LacunarityItem = cJSON_GetObjectItemCaseSensitive(Json, "lacunarity");
  if (cJSON_IsNumber(LacunarityItem) != 0) {
    Params->Lacunarity = (float)LacunarityItem->valuedouble;
  }

  cJSON *GainItem = cJSON_GetObjectItemCaseSensitive(Json, "gain");
  if (cJSON_IsNumber(GainItem) != 0) {
    Params->Gain = (float)GainItem->valuedouble;
  }

  cJSON *OctavesItem = cJSON_GetObjectItemCaseSensitive(Json, "octaves");
  if (cJSON_IsNumber(OctavesItem) != 0 && OctavesItem->valueint > 0) {
    Params->Octaves = OctavesItem->valueint;
  }

  LogInfo("BIOME: noise params scale=%.4f octaves=%d lacunarity=%.2f gain=%.2f",
          (double)Params->Scale, Params->Octaves, (double)Params->Lacunarity,
          (double)Params->Gain);

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

  #pragma unroll 4
  for (int Idx = 0; Idx < FilesCount; Idx++) {
    if (HasFileExtension(FilePaths[Idx], ".json")) {
      ParseBiomeFile(FilePaths[Idx]);
    }
  }

  int LoadedCount = GetLoadedBiomesCount();
  if (LoadedCount == 0) {
    LogWarn("BIOME: No valid biome loaded from %s", Path);
  } else {
    LogInfo("BIOME: Loaded %d biome definitions", LoadedCount);
  }
}
