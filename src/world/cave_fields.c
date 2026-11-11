#include "world/cave_fields.h"
#include "core/fileio.h"
#include "core/log.h"
#include "core/noise.h"
#include "third_party/cJSON.h"
#include "world/chunk.h"
#include "world/terrain_density.h"
#include "world/terrain_shape.h"
#include "world/worldgen_strata.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>

typedef enum {
  CAVE_FIELD_CHEESE = 0,
  CAVE_FIELD_SPAGHETTI_PATH,
  CAVE_FIELD_SPAGHETTI_ELEVATION,
  CAVE_FIELD_SPAGHETTI_WIDTH,
  CAVE_FIELD_NOODLE_A,
  CAVE_FIELD_NOODLE_B,
  CAVE_FIELD_NOODLE_TOGGLE,
  CAVE_FIELD_ENTRANCE,
  CAVE_FIELD_FLOOD,
  CAVE_FIELD_COUNT
} CaveField;

typedef struct {
  CaveParams Params;
  NoiseFieldDesc Descs[CAVE_FIELD_COUNT];
  float VerticalScale[CAVE_FIELD_COUNT];
  NoiseField Fields[CAVE_FIELD_COUNT];
} CaveState;

static CaveState *GetCaveState(void) {
  static CaveState SState;
  return &SState;
}

static const char *const CAVE_FIELD_KEYS[CAVE_FIELD_COUNT] = {
    "caveCheese",       "caveSpagPath", "caveSpagElevation",
    "caveSpagWidth",    "caveNoodleA",  "caveNoodleB",
    "caveNoodleToggle", "caveEntrance", "caveFlood"};

static const uint64_t CAVE_FIELD_SALTS[CAVE_FIELD_COUNT] = {
    0xCA5E000000000001ULL, 0xCA5E000000000002ULL, 0xCA5E000000000003ULL,
    0xCA5E000000000004ULL, 0xCA5E000000000005ULL, 0xCA5E000000000006ULL,
    0xCA5E000000000007ULL, 0xCA5E000000000008ULL, 0xCA5E000000000009ULL};

// Inside the shell the Caves term is (Shell - depth) * SHELL_K: positive, so the
// terrain stays solid there. One block of density per block keeps it on the
// terrain's own scale.
#define SHELL_K 1.0F
// How hard the floor fade closes caves: at the cave floor it adds
// FloorFade * FADE_K blocks, more than any cave term can subtract.
#define FADE_K 4.0F

// ---------------------------------------------------------------------------
// Defaults and loading
// ---------------------------------------------------------------------------

// Used when caves.json is missing or rejected; mirrors the shipped file.
static void InstallDefaultParams(CaveParams *Params) {
  static const CaveParams DEFAULT_PARAMS = {
      .CheeseEnabled = true,
      .SpaghettiEnabled = true,
      .NoodleEnabled = true,
      .ThresholdCount = 8,
      .ThresholdY = {-511.0F, -470.0F, -300.0F, -160.0F, -60.0F, 0.0F, 200.0F,
                     640.0F},
      .ThresholdValue = {0.6F, 0.22F, 0.18F, 0.25F, 0.35F, 0.45F, 0.5F, 0.6F},
      .SpaghettiWidth = 0.1F,
      .SpaghettiWidthVariation = 0.02F,
      .SpaghettiLayerSpacing = 48,
      .SpaghettiHalfHeight = 5.0F,
      .SpaghettiElevationAmplitude = 10.0F,
      .NoodleWidth = 0.035F,
      .NoodleToggle = 0.3F,
      .CaveScale = 32.0F,
      .ShellBase = 10.0F,
      .ShellUnderwater = 26.0F,
      .EntranceDepth = 14.0F,
      .CaveFloor = -508,
      .FloorFade = 12.0F,
      .LavaLevel = -480,
      .FloodThreshold = 0.45F,
      .BarrierBand = 0.03F};
  *Params = DEFAULT_PARAMS;
}

static void InstallDefaultFields(CaveState *State) {
  const struct {
    int Octaves;
    float Frequency;
    bool ImproveXZ;
    float VerticalScale;
  } DEFAULTS[CAVE_FIELD_COUNT] = {
      {2, 0.008F, true, 1.6F},  {1, 0.008F, false, 1.0F}, {1, 0.008F, false, 1.0F},
      {1, 0.01F, false, 1.0F},  {1, 0.05F, true, 1.0F},  {1, 0.05F, true, 1.0F},
      {1, 0.008F, true, 1.0F},  {1, 0.01F, false, 1.0F}, {1, 0.006F, true, 1.0F}};

  for (int Field = 0; Field < CAVE_FIELD_COUNT; Field++) {
    NoiseFieldDesc Desc = NoiseDefaultDesc();
    Desc.Octaves = DEFAULTS[Field].Octaves;
    Desc.Frequency = DEFAULTS[Field].Frequency;
    Desc.ImproveXZ = DEFAULTS[Field].ImproveXZ;
    Desc.Fractal = (Desc.Octaves > 1) ? NOISE_FRACTAL_FBM : NOISE_FRACTAL_NONE;
    State->Descs[Field] = Desc;
    State->VerticalScale[Field] = DEFAULTS[Field].VerticalScale;
  }
}

static void DefineFields(CaveState *State) {
  for (int Field = 0; Field < CAVE_FIELD_COUNT; Field++) {
    if (!NoiseFieldDefine(&State->Fields[Field], &State->Descs[Field],
                          CAVE_FIELD_SALTS[Field])) {
      LogError("CAVES: noise field table is full; field '%s' samples as 0",
               CAVE_FIELD_KEYS[Field]);
    }
  }
}

void InitCaveFields(void) {
  CaveState *State = GetCaveState();
  for (int Field = 0; Field < CAVE_FIELD_COUNT; Field++) {
    State->Fields[Field] = NOISE_FIELD_INVALID;
  }
  InstallDefaultParams(&State->Params);
  InstallDefaultFields(State);
  DefineFields(State);
}

void LoadCaveNoise(const char *FilePath) {
  CaveState *State = GetCaveState();
  char *Content = ReadTextFile(FilePath);
  cJSON *Json = (Content != NULL) ? cJSON_Parse(Content) : NULL;
  free(Content);
  if (Json == NULL) {
    LogWarn("CAVES: could not read %s, keeping default cave fields", FilePath);
    return;
  }

  for (int Field = 0; Field < CAVE_FIELD_COUNT; Field++) {
    const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, CAVE_FIELD_KEYS[Field]);
    if (cJSON_IsObject(Item) == 0) {
      LogWarn("CAVES: %s has no \"%s\" field, keeping its default", FilePath,
              CAVE_FIELD_KEYS[Field]);
      continue;
    }
    ReadNoiseFieldDesc(Item, &State->Descs[Field], FilePath);
    const cJSON *Scale = cJSON_GetObjectItemCaseSensitive(Item, "verticalScale");
    if (cJSON_IsNumber(Scale) != 0) {
      State->VerticalScale[Field] = (float)Scale->valuedouble;
    }
  }
  cJSON_Delete(Json);
  DefineFields(State);
}

static void ReadBool(const cJSON *Json, const char *Key, bool *Out) {
  const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, Key);
  if (cJSON_IsBool(Item) != 0) {
    *Out = cJSON_IsTrue(Item) != 0;
  }
}

static void ReadFloat(const cJSON *Json, const char *Key, float *Out) {
  const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, Key);
  if (cJSON_IsNumber(Item) != 0) {
    *Out = (float)Item->valuedouble;
  }
}

static void ReadInt(const cJSON *Json, const char *Key, int *Out) {
  const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Json, Key);
  if (cJSON_IsNumber(Item) != 0) {
    *Out = Item->valueint;
  }
}

static bool ParseThresholds(const cJSON *Json, CaveParams *Params,
                            const char *FilePath) {
  const cJSON *Table = cJSON_GetObjectItemCaseSensitive(Json, "cheeseThresholdByY");
  if (Table == NULL) {
    return true;
  }
  int Count = (cJSON_IsArray(Table) != 0) ? cJSON_GetArraySize(Table) : 0;
  if (Count < 1 || Count > CAVE_MAX_THRESHOLDS) {
    LogError("CAVES: %s cheeseThresholdByY must have 1..%d [y, threshold] "
             "pairs",
             FilePath, CAVE_MAX_THRESHOLDS);
    return false;
  }
  for (int Idx = 0; Idx < Count; Idx++) {
    const cJSON *Pair = cJSON_GetArrayItem(Table, Idx);
    const cJSON *Y = cJSON_GetArrayItem(Pair, 0);
    const cJSON *Value = cJSON_GetArrayItem(Pair, 1);
    if (cJSON_IsNumber(Y) == 0 || cJSON_IsNumber(Value) == 0) {
      LogError("CAVES: %s cheeseThresholdByY entry %d is not a [y, threshold] "
               "pair",
               FilePath, Idx);
      return false;
    }
    Params->ThresholdY[Idx] = (float)Y->valuedouble;
    Params->ThresholdValue[Idx] = (float)Value->valuedouble;
    if (Idx > 0 && !(Params->ThresholdY[Idx] > Params->ThresholdY[Idx - 1])) {
      LogError("CAVES: %s cheeseThresholdByY has non-increasing Y at entry %d",
               FilePath, Idx);
      return false;
    }
  }
  Params->ThresholdCount = Count;
  return true;
}

static bool ValidateParams(const CaveParams *Params, const char *FilePath) {
  int OverworldFloor = 0;
  int Unused = 0;
  if (!GetStratumRange(STRATUM_GEN_OVERWORLD, &OverworldFloor, &Unused)) {
    OverworldFloor = Params->CaveFloor;
  }
  // A band must hold its tunnel plus one grid cell of rock on each side,
  // wherever the elevation field puts it, so no corner near a band edge would
  // have needed the neighbouring band's tunnel.
  float HalfBandNeed = Params->SpaghettiHalfHeight +
                       (Params->SpaghettiElevationAmplitude *
                        TERRAIN_NOISE_BOUND) +
                       (float)DENSITY_CELL_Y;
  float BandNeed = HalfBandNeed + HalfBandNeed;
  bool Ok = Params->SpaghettiWidth >= 0.0F &&
            Params->SpaghettiWidthVariation >= 0.0F &&
            Params->SpaghettiHalfHeight > (float)DENSITY_CELL_Y / 2 &&
            Params->SpaghettiElevationAmplitude >= 0.0F &&
            (float)Params->SpaghettiLayerSpacing >= BandNeed &&
            Params->NoodleWidth >= 0.0F && Params->CaveScale > 0.0F &&
            Params->ShellBase >= 0.0F &&
            Params->ShellUnderwater >= Params->ShellBase &&
            Params->EntranceDepth >= 0.0F &&
            Params->FloorFade >= 1.0F && Params->BarrierBand > 0.0F &&
            Params->CaveFloor >= OverworldFloor;
  if (!Ok) {
    LogError("CAVES: %s has a negative width or depth, a non-positive "
             "caveScale or barrierBand, floorFade below 1, caveFloor below the overworld "
             "floor (%d), spaghettiHalfHeight not above %d, or "
             "spaghettiLayerSpacing below %.1f",
             FilePath, OverworldFloor, DENSITY_CELL_Y / 2, (double)BandNeed);
  }
  return Ok;
}

void LoadCaveParams(const char *FilePath) {
  CaveState *State = GetCaveState();
  char *Content = ReadTextFile(FilePath);
  if (Content == NULL) {
    LogWarn("CAVES: no parameters at %s, using built-in defaults", FilePath);
    InstallDefaultParams(&State->Params);
    return;
  }
  cJSON *Json = cJSON_Parse(Content);
  free(Content);
  if (Json == NULL) {
    LogError("CAVES: JSON syntax error in %s, using built-in defaults",
             FilePath);
    InstallDefaultParams(&State->Params);
    return;
  }

  CaveParams Parsed;
  InstallDefaultParams(&Parsed);
  ReadBool(Json, "cheese", &Parsed.CheeseEnabled);
  ReadBool(Json, "spaghetti", &Parsed.SpaghettiEnabled);
  ReadBool(Json, "noodle", &Parsed.NoodleEnabled);
  ReadFloat(Json, "spaghettiWidth", &Parsed.SpaghettiWidth);
  ReadFloat(Json, "spaghettiWidthVariation", &Parsed.SpaghettiWidthVariation);
  ReadInt(Json, "spaghettiLayerSpacing", &Parsed.SpaghettiLayerSpacing);
  ReadFloat(Json, "spaghettiHalfHeight", &Parsed.SpaghettiHalfHeight);
  ReadFloat(Json, "spaghettiElevationAmplitude",
            &Parsed.SpaghettiElevationAmplitude);
  ReadFloat(Json, "noodleWidth", &Parsed.NoodleWidth);
  ReadFloat(Json, "noodleToggle", &Parsed.NoodleToggle);
  ReadFloat(Json, "caveScale", &Parsed.CaveScale);
  ReadFloat(Json, "shellBase", &Parsed.ShellBase);
  ReadFloat(Json, "shellUnderwater", &Parsed.ShellUnderwater);
  ReadFloat(Json, "entranceDepth", &Parsed.EntranceDepth);
  ReadInt(Json, "caveFloor", &Parsed.CaveFloor);
  ReadFloat(Json, "floorFade", &Parsed.FloorFade);
  ReadInt(Json, "lavaLevel", &Parsed.LavaLevel);
  ReadFloat(Json, "floodThreshold", &Parsed.FloodThreshold);
  ReadFloat(Json, "barrierBand", &Parsed.BarrierBand);

  if (ParseThresholds(Json, &Parsed, FilePath) &&
      ValidateParams(&Parsed, FilePath)) {
    State->Params = Parsed;
    LogInfo("CAVES: cheese=%d spaghetti=%d noodle=%d floor=%d lava=%d",
            Parsed.CheeseEnabled, Parsed.SpaghettiEnabled, Parsed.NoodleEnabled,
            Parsed.CaveFloor, Parsed.LavaLevel);
  } else {
    LogError("CAVES: %s rejected, using built-in defaults", FilePath);
    InstallDefaultParams(&State->Params);
  }
  cJSON_Delete(Json);
}

const CaveParams *GetCaveParams(void) { return &GetCaveState()->Params; }

bool CavesEnabled(void) {
  const CaveParams *Params = &GetCaveState()->Params;
  return Params->CheeseEnabled || Params->SpaghettiEnabled ||
         Params->NoodleEnabled;
}

// ---------------------------------------------------------------------------
// Fields
// ---------------------------------------------------------------------------

static float SampleCaveField(CaveField Field, int X, int Y, int Z,
                             uint64_t Seed) {
  const CaveState *State = GetCaveState();
  return NoiseSample3D(State->Fields[Field], Seed, (float)X,
                       (float)Y * State->VerticalScale[Field], (float)Z);
}

// Piecewise-linear in Y, clamped to the end values.
static float CheeseThreshold(const CaveParams *Params, int Y) {
  float At = (float)Y;
  int Last = Params->ThresholdCount - 1;
  if (At <= Params->ThresholdY[0]) {
    return Params->ThresholdValue[0];
  }
  if (At >= Params->ThresholdY[Last]) {
    return Params->ThresholdValue[Last];
  }
  int Segment = 0;
  while (Segment < Last - 1 && At >= Params->ThresholdY[Segment + 1]) {
    Segment++;
  }
  float Span = Params->ThresholdY[Segment + 1] - Params->ThresholdY[Segment];
  float T = (At - Params->ThresholdY[Segment]) / Span;
  return Params->ThresholdValue[Segment] +
         ((Params->ThresholdValue[Segment + 1] - Params->ThresholdValue[Segment]) *
          T);
}

// Where water can stand within ShellBase of the surface (sea floors, low
// coasts) the shell is ShellUnderwater, so no cave reaches the water band. On
// higher land the entrance field thins it so caves occasionally reach the
// surface.
float CaveShellDepth(int LatticeX, int LatticeZ, float Base, uint64_t Seed) {
  const CaveState *State = GetCaveState();
  const CaveParams *Params = &State->Params;
  const float LAND_MARGIN = 2.0F;
  if (Base <= (float)SEA_LEVEL + Params->ShellBase + LAND_MARGIN) {
    return Params->ShellUnderwater;
  }
  if (Params->EntranceDepth <= 0.0F) {
    return Params->ShellBase;
  }
  float Entrance = NoiseSample2D(State->Fields[CAVE_FIELD_ENTRANCE], Seed,
                                 (float)LatticeX, (float)LatticeZ);
  float Shell = Params->ShellBase - (Params->EntranceDepth * fmaxf(0.0F, Entrance));
  return fmaxf(0.0F, Shell);
}

// 2D field of one spaghetti band. Each band reads the field at its own far
// offset, so bands are uncorrelated.
static float SampleBandField(CaveField Field, int X, int Z, int Band,
                             uint64_t Seed) {
  const float BAND_SHIFT_X = 1733.0F;
  const float BAND_SHIFT_Z = -2659.0F;
  return NoiseSample2D(GetCaveState()->Fields[Field], Seed,
                       (float)X + ((float)Band * BAND_SHIFT_X),
                       (float)Z + ((float)Band * BAND_SHIFT_Z));
}

// min(Raw, spaghetti). The tunnel is max(path, vertical): open within Width of
// the path field's zero line and within SpaghettiHalfHeight of the band's
// elevation. The vertical term alone bounds it from below, so where it already
// reaches Raw the path and width fields are not sampled.
static float AddSpaghetti(const CaveParams *Params, float Raw, int X, int Y,
                          int Z, uint64_t Seed) {
  float Spacing = (float)Params->SpaghettiLayerSpacing;
  int Band = (int)floorf((float)Y / Spacing);
  float Elevation =
      SampleBandField(CAVE_FIELD_SPAGHETTI_ELEVATION, X, Z, Band, Seed);
  float Center = ((float)Band * Spacing) + (Spacing / 2) +
                 (Elevation * Params->SpaghettiElevationAmplitude);
  float Vertical = fabsf((float)Y - Center) - Params->SpaghettiHalfHeight;
  if (Vertical >= Raw) {
    return Raw;
  }
  float Width = Params->SpaghettiWidth;
  if (Params->SpaghettiWidthVariation > 0.0F) {
    Width += Params->SpaghettiWidthVariation *
             SampleBandField(CAVE_FIELD_SPAGHETTI_WIDTH, X, Z, Band, Seed);
  }
  float Path = SampleBandField(CAVE_FIELD_SPAGHETTI_PATH, X, Z, Band, Seed);
  float Horizontal = (fabsf(Path) - Width) * Params->CaveScale;
  return fminf(Raw, fmaxf(Horizontal, Vertical));
}

// Open where both fields are within Width of zero: the intersection of two
// zero sheets is a long winding tube.
static float TunnelDensity(CaveField FieldA, CaveField FieldB, float Width,
                           int X, int Y, int Z, uint64_t Seed) {
  float A = fabsf(SampleCaveField(FieldA, X, Y, Z, Seed));
  float B = fabsf(SampleCaveField(FieldB, X, Y, Z, Seed));
  return (fmaxf(A, B) - Width) * GetCaveState()->Params.CaveScale;
}

float CaveCornerDensity(float Base, float ShellDepth, int X, int Y, int Z,
                        uint64_t Seed) {
  const CaveParams *Params = &GetCaveState()->Params;
  if (!CavesEnabled()) {
    return FLT_MAX;
  }

  float DepthBelowBase = Base - (float)Y;
  if (DepthBelowBase < ShellDepth) {
    return (ShellDepth - DepthBelowBase) * SHELL_K;
  }
  if (Y < Params->CaveFloor) {
    return FLT_MAX;
  }

  float Raw = FLT_MAX;
  if (Params->CheeseEnabled) {
    float Cheese = SampleCaveField(CAVE_FIELD_CHEESE, X, Y, Z, Seed);
    Raw = (CheeseThreshold(Params, Y) - Cheese) * Params->CaveScale;
  }
  // A tunnel's density never falls below -(its widest width) * CaveScale, nor
  // a spaghetti tunnel's below -SpaghettiHalfHeight. Where the corner is
  // already more open than that, a tunnel cannot lower the minimum, so its
  // fields are not sampled. The probe takes the same branch, so this changes
  // nothing it can observe.
  float SpaghettiFloor =
      fmaxf(-(Params->SpaghettiWidth +
              (Params->SpaghettiWidthVariation * TERRAIN_NOISE_BOUND)) *
                Params->CaveScale,
            -Params->SpaghettiHalfHeight);
  if (Params->SpaghettiEnabled && Raw > SpaghettiFloor) {
    Raw = AddSpaghetti(Params, Raw, X, Y, Z, Seed);
  }
  // The toggle is sampled first: where noodles are off their pair of fields is
  // never sampled.
  float NoodleFloor = -Params->NoodleWidth * Params->CaveScale;
  if (Params->NoodleEnabled && Raw > NoodleFloor &&
      SampleCaveField(CAVE_FIELD_NOODLE_TOGGLE, X, Y, Z, Seed) >
          Params->NoodleToggle) {
    Raw = fminf(Raw, TunnelDensity(CAVE_FIELD_NOODLE_A, CAVE_FIELD_NOODLE_B,
                                   Params->NoodleWidth, X, Y, Z, Seed));
  }
  if (Raw == FLT_MAX) {
    return FLT_MAX;
  }

  float FadeTop = (float)Params->CaveFloor + Params->FloorFade;
  if ((float)Y < FadeTop) {
    Raw += (FadeTop - (float)Y) * FADE_K;
  }
  return Raw;
}

float CaveFloodLattice(int X, int Y, int Z, uint64_t Seed) {
  return SampleCaveField(CAVE_FIELD_FLOOD, X, Y, Z, Seed);
}

bool CaveFloodPossible(int Y) {
  const CaveParams *Params = &GetCaveState()->Params;
  return CavesEnabled() && Y <= SEA_LEVEL && Y > Params->LavaLevel;
}

bool CaveFloodZone(int Y, int Surface) {
  return CaveFloodPossible(Y) &&
         (float)Y < (float)Surface - GetCaveState()->Params.ShellBase;
}

bool CaveFloodRows(int Surface, int *Lowest, int *Highest) {
  const CaveParams *Params = &GetCaveState()->Params;
  if (!CavesEnabled()) {
    return false;
  }
  // The highest Y with (float)Y < Limit, found with the same comparison
  // CaveFloodZone makes, so both agree even where rounding is involved.
  float Limit = (float)Surface - Params->ShellBase;
  int High = (int)ceilf(Limit) - 1;
  while (!((float)High < Limit)) {
    High--;
  }
  while ((float)(High + 1) < Limit) {
    High++;
  }
  *Lowest = Params->LavaLevel + 1;
  *Highest = (High < SEA_LEVEL) ? High : SEA_LEVEL;
  return *Lowest <= *Highest;
}

float CaveBarrierDensity(float Flood) {
  const CaveParams *Params = &GetCaveState()->Params;
  return (Params->BarrierBand - fabsf(Flood - Params->FloodThreshold)) *
         Params->CaveScale;
}

// Oceans and lakes fill only the band within ShellBase of the surface, which
// caves never reach, so a cave below sea level is not flooded by the sea.
CaveFluid CaveFluidAt(int Y, int Surface, bool Flooded) {
  const CaveParams *Params = &GetCaveState()->Params;
  if (Y <= SEA_LEVEL && (float)Y >= (float)Surface - Params->ShellBase) {
    return CAVE_FLUID_WATER;
  }
  if (Y <= Params->LavaLevel) {
    return CAVE_FLUID_LAVA;
  }
  return Flooded ? CAVE_FLUID_WATER : CAVE_FLUID_AIR;
}
