#include "world/terrain_shape.h"
#include "core/fileio.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/noise.h"
#include "third_party/cJSON.h"
#include "world/cave_fields.h"
#include "world/shape_modifiers.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  MAX_SPLINES = 128,
  MAX_SPLINE_POINTS = 256,
  MAX_POINTS_PER_SPLINE = 16,
  MAX_SPLINE_DEPTH = 8,
  SPLINE_NONE = -1
};

typedef enum {
  SHAPE_FIELD_CONTINENTALNESS = 0,
  SHAPE_FIELD_EROSION,
  SHAPE_FIELD_WEIRDNESS,
  SHAPE_FIELD_JAGGED,
  SHAPE_FIELD_TERRAIN,
  SHAPE_FIELD_COUNT
} ShapeField;

typedef struct {
  float Location;
  float Value;
  float Derivative;
  // Index of a nested spline giving this point's value, or SPLINE_NONE.
  int Child;
} SplinePoint;

typedef struct {
  ShapeCoordinate Coordinate;
  int FirstPoint;
  int PointCount;
} Spline;

typedef struct {
  Spline Splines[MAX_SPLINES];
  int SplineCount;
  SplinePoint Points[MAX_SPLINE_POINTS];
  int PointCount;
  int BaseRoot;
  int VariationRoot;
  int JaggedRoot;
  float CeilingTaper;
} ShapeData;

typedef struct {
  ShapeData Data;
  NoiseFieldDesc FieldDescs[SHAPE_FIELD_COUNT];
  NoiseField Fields[SHAPE_FIELD_COUNT];
  float TerrainVerticalScale;
} ShapeState;

static ShapeState *GetShapeState(void) {
  static ShapeState SState;
  return &SState;
}

static const char *const FIELD_KEYS[SHAPE_FIELD_COUNT] = {
    "continentalness", "erosion", "weirdness", "jagged", "terrain"};

static const uint64_t FIELD_SALTS[SHAPE_FIELD_COUNT] = {
    0x5A4E0C0000000001ULL, 0x5A4E0C0000000002ULL, 0x5A4E0C0000000003ULL,
    0x5A4E0C0000000004ULL, 0x5A4E0C0000000005ULL};

static const char *const COORD_NAMES[SHAPE_COORD_COUNT] = {
    "continentalness", "erosion", "weirdness", "peaks_valleys"};

// ---------------------------------------------------------------------------
// Noise fields
// ---------------------------------------------------------------------------

static void InstallDefaultFields(ShapeState *State) {
  const struct {
    int Octaves;
    float Frequency;
    bool ImproveXZ;
  } DEFAULTS[SHAPE_FIELD_COUNT] = {{5, 0.0008F, false},
                                   {4, 0.0015F, false},
                                   {4, 0.002F, false},
                                   {2, 0.08F, false},
                                   {4, 0.012F, true}};
  const float DEFAULT_VERTICAL_SCALE = 1.5F;

  for (int Field = 0; Field < SHAPE_FIELD_COUNT; Field++) {
    NoiseFieldDesc Desc = NoiseDefaultDesc();
    Desc.Octaves = DEFAULTS[Field].Octaves;
    Desc.Frequency = DEFAULTS[Field].Frequency;
    Desc.ImproveXZ = DEFAULTS[Field].ImproveXZ;
    State->FieldDescs[Field] = Desc;
  }
  State->TerrainVerticalScale = DEFAULT_VERTICAL_SCALE;
}

static void DefineFields(ShapeState *State) {
  for (int Field = 0; Field < SHAPE_FIELD_COUNT; Field++) {
    if (!NoiseFieldDefine(&State->Fields[Field], &State->FieldDescs[Field],
                          FIELD_SALTS[Field])) {
      LogError("SHAPE: noise field table is full; field '%s' samples as 0",
               FIELD_KEYS[Field]);
    }
  }
}

static void ReadFloat(const cJSON *Object, const char *Key, float *Out) {
  const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Object, Key);
  if (cJSON_IsNumber(Item) != 0) {
    *Out = (float)Item->valuedouble;
  }
}

void ReadNoiseFieldDesc(const cJSON *Object, NoiseFieldDesc *Desc,
                        const char *FilePath) {
  const cJSON *KindItem = cJSON_GetObjectItemCaseSensitive(Object, "kind");
  if (cJSON_IsString(KindItem) != 0 &&
      !NoiseParseKind(KindItem->valuestring, &Desc->Kind)) {
    LogError("SHAPE: unknown noise kind '%s' in %s, keeping default",
             KindItem->valuestring, FilePath);
  }

  const cJSON *FractalItem = cJSON_GetObjectItemCaseSensitive(Object, "fractal");
  if (cJSON_IsString(FractalItem) != 0 &&
      !NoiseParseFractal(FractalItem->valuestring, &Desc->Fractal)) {
    LogError("SHAPE: unknown fractal '%s' in %s, keeping default",
             FractalItem->valuestring, FilePath);
  }

  const cJSON *OctavesItem = cJSON_GetObjectItemCaseSensitive(Object, "octaves");
  if (cJSON_IsNumber(OctavesItem) != 0 && OctavesItem->valueint > 0) {
    Desc->Octaves = OctavesItem->valueint;
  }

  ReadFloat(Object, "frequency", &Desc->Frequency);
  ReadFloat(Object, "lacunarity", &Desc->Lacunarity);
  ReadFloat(Object, "gain", &Desc->Gain);

  const cJSON *RotateItem = cJSON_GetObjectItemCaseSensitive(Object, "improveXZ");
  if (cJSON_IsBool(RotateItem) != 0) {
    Desc->ImproveXZ = cJSON_IsTrue(RotateItem) != 0;
  }
}

static cJSON *ParseJsonFile(const char *FilePath, const char *Missing) {
  char *FileContent = ReadTextFile(FilePath);
  if (FileContent == NULL) {
    LogWarn("SHAPE: no file at %s, %s", FilePath, Missing);
    return NULL;
  }
  cJSON *Json = cJSON_Parse(FileContent);
  free(FileContent);
  if (Json == NULL) {
    LogError("SHAPE: JSON syntax error in %s, %s", FilePath, Missing);
  }
  return Json;
}

void LoadTerrainNoise(const char *FilePath) {
  ShapeState *State = GetShapeState();
  cJSON *Json = ParseJsonFile(FilePath, "keeping default noise fields");
  if (Json != NULL) {
    for (int Field = 0; Field < SHAPE_FIELD_COUNT; Field++) {
      const cJSON *Item =
          cJSON_GetObjectItemCaseSensitive(Json, FIELD_KEYS[Field]);
      if (cJSON_IsObject(Item) != 0) {
        ReadNoiseFieldDesc(Item, &State->FieldDescs[Field], FilePath);
      } else {
        LogWarn("SHAPE: %s has no \"%s\" field, keeping its default", FilePath,
                FIELD_KEYS[Field]);
      }
    }
    const cJSON *Terrain = cJSON_GetObjectItemCaseSensitive(
        Json, FIELD_KEYS[SHAPE_FIELD_TERRAIN]);
    if (cJSON_IsObject(Terrain) != 0) {
      ReadFloat(Terrain, "verticalScale", &State->TerrainVerticalScale);
    }
    cJSON_Delete(Json);
  }
  DefineFields(State);
}

// ---------------------------------------------------------------------------
// Splines
// ---------------------------------------------------------------------------

static int AddConstantSpline(ShapeData *Data, float Value) {
  int Index = Data->SplineCount++;
  Data->Splines[Index] = (Spline){.Coordinate = SHAPE_COORD_CONTINENTALNESS,
                                  .FirstPoint = Data->PointCount,
                                  .PointCount = 1};
  Data->Points[Data->PointCount++] =
      (SplinePoint){.Location = 0.0F, .Value = Value, .Derivative = 0.0F,
                    .Child = SPLINE_NONE};
  return Index;
}

// Fallback when terrain_shape.json is missing or rejected: oceans below zero
// continentalness rising to low land above it, constant variation, no jags.
static void InstallDefaultShape(ShapeData *Data) {
  const float LOCATIONS[] = {-0.6F, -0.2F, -0.05F, 0.1F, 0.6F};
  const float HEIGHTS[] = {-40.0F, -12.0F, 2.0F, 12.0F, 40.0F};
  const int COUNT = (int)(sizeof(LOCATIONS) / sizeof(LOCATIONS[0]));
  const float DEFAULT_VARIATION = 4.0F;
  const float DEFAULT_CEILING_TAPER = 64.0F;

  memset(Data, 0, sizeof(*Data));
  Data->BaseRoot = Data->SplineCount++;
  Data->Splines[Data->BaseRoot] =
      (Spline){.Coordinate = SHAPE_COORD_CONTINENTALNESS,
               .FirstPoint = Data->PointCount,
               .PointCount = COUNT};
  for (int Idx = 0; Idx < COUNT; Idx++) {
    Data->Points[Data->PointCount++] =
        (SplinePoint){.Location = LOCATIONS[Idx], .Value = HEIGHTS[Idx],
                      .Derivative = 0.0F, .Child = SPLINE_NONE};
  }
  Data->VariationRoot = AddConstantSpline(Data, DEFAULT_VARIATION);
  Data->JaggedRoot = AddConstantSpline(Data, 0.0F);
  Data->CeilingTaper = DEFAULT_CEILING_TAPER;
}

typedef struct {
  ShapeData *Data;
  const char *FilePath;
  const char *Root;
} ParseContext;

static bool ParseCoordinate(const char *Name, ShapeCoordinate *Out) {
  for (int Coord = 0; Coord < SHAPE_COORD_COUNT; Coord++) {
    if (strcmp(Name, COORD_NAMES[Coord]) == 0) {
      *Out = (ShapeCoordinate)Coord;
      return true;
    }
  }
  return false;
}

static bool ParseSpline(const ParseContext *Ctx, const cJSON *Node, int Depth,
                        int *OutIndex);

static bool ParsePoint(const ParseContext *Ctx, const cJSON *Item, int Depth,
                       SplinePoint *Out) {
  const cJSON *Location = cJSON_GetObjectItemCaseSensitive(Item, "location");
  const cJSON *Value = cJSON_GetObjectItemCaseSensitive(Item, "value");
  const cJSON *Derivative =
      cJSON_GetObjectItemCaseSensitive(Item, "derivative");

  if (cJSON_IsNumber(Location) == 0 ||
      (Derivative != NULL && cJSON_IsNumber(Derivative) == 0)) {
    LogError("SHAPE: %s spline '%s' has a point with a non-numeric location or "
             "derivative",
             Ctx->FilePath, Ctx->Root);
    return false;
  }

  *Out = (SplinePoint){.Location = (float)Location->valuedouble,
                       .Value = 0.0F,
                       .Derivative = (Derivative != NULL)
                                         ? (float)Derivative->valuedouble
                                         : 0.0F,
                       .Child = SPLINE_NONE};
  if (cJSON_IsNumber(Value) != 0) {
    Out->Value = (float)Value->valuedouble;
    return true;
  }
  if (cJSON_IsObject(Value) != 0) {
    return ParseSpline(Ctx, Value, Depth + 1, &Out->Child);
  }
  LogError("SHAPE: %s spline '%s' has a point whose value is neither a number "
           "nor a spline",
           Ctx->FilePath, Ctx->Root);
  return false;
}

// A spline's points are reserved as one contiguous block before any nested
// spline is parsed, so children allocate after it and never interleave.
static bool ParseSpline(const ParseContext *Ctx, const cJSON *Node, int Depth,
                        int *OutIndex) {
  ShapeData *Data = Ctx->Data;
  if (Depth > MAX_SPLINE_DEPTH) {
    LogError("SHAPE: %s spline '%s' nests deeper than %d", Ctx->FilePath,
             Ctx->Root, MAX_SPLINE_DEPTH);
    return false;
  }

  const cJSON *CoordItem = cJSON_GetObjectItemCaseSensitive(Node, "coordinate");
  ShapeCoordinate Coordinate = SHAPE_COORD_CONTINENTALNESS;
  if (cJSON_IsString(CoordItem) == 0 ||
      !ParseCoordinate(CoordItem->valuestring, &Coordinate)) {
    LogError("SHAPE: %s spline '%s' has unknown coordinate '%s'", Ctx->FilePath,
             Ctx->Root,
             (cJSON_IsString(CoordItem) != 0) ? CoordItem->valuestring : "?");
    return false;
  }

  const cJSON *PointsItem = cJSON_GetObjectItemCaseSensitive(Node, "points");
  int Count = (cJSON_IsArray(PointsItem) != 0) ? cJSON_GetArraySize(PointsItem)
                                               : 0;
  if (Count < 1 || Count > MAX_POINTS_PER_SPLINE) {
    LogError("SHAPE: %s spline '%s' must have 1..%d points, has %d",
             Ctx->FilePath, Ctx->Root, MAX_POINTS_PER_SPLINE, Count);
    return false;
  }
  if (Data->SplineCount >= MAX_SPLINES ||
      Data->PointCount + Count > MAX_SPLINE_POINTS) {
    LogError("SHAPE: %s exceeds %d points in total", Ctx->FilePath,
             MAX_SPLINE_POINTS);
    return false;
  }

  int Index = Data->SplineCount++;
  int First = Data->PointCount;
  Data->PointCount += Count;
  Data->Splines[Index] =
      (Spline){.Coordinate = Coordinate, .FirstPoint = First, .PointCount = Count};

  for (int Idx = 0; Idx < Count; Idx++) {
    SplinePoint Point;
    if (!ParsePoint(Ctx, cJSON_GetArrayItem(PointsItem, Idx), Depth, &Point)) {
      return false;
    }
    if (Idx > 0 && !(Point.Location > Data->Points[First + Idx - 1].Location)) {
      LogError("SHAPE: %s spline '%s' has non-increasing locations (%g after "
               "%g)",
               Ctx->FilePath, Ctx->Root, (double)Point.Location,
               (double)Data->Points[First + Idx - 1].Location);
      return false;
    }
    Data->Points[First + Idx] = Point;
  }

  *OutIndex = Index;
  return true;
}

static bool ParseShapeFile(const cJSON *Json, ShapeData *Data,
                           const char *FilePath) {
  const char *const ROOT_KEYS[] = {"baseHeight", "variation", "jaggedness"};
  int *Roots[] = {&Data->BaseRoot, &Data->VariationRoot, &Data->JaggedRoot};

  memset(Data, 0, sizeof(*Data));
  for (size_t Idx = 0; Idx < sizeof(ROOT_KEYS) / sizeof(ROOT_KEYS[0]); Idx++) {
    const cJSON *Node = cJSON_GetObjectItemCaseSensitive(Json, ROOT_KEYS[Idx]);
    if (cJSON_IsObject(Node) == 0) {
      LogError("SHAPE: %s has no \"%s\" spline", FilePath, ROOT_KEYS[Idx]);
      return false;
    }
    ParseContext Ctx = {.Data = Data, .FilePath = FilePath, .Root = ROOT_KEYS[Idx]};
    if (!ParseSpline(&Ctx, Node, 1, Roots[Idx])) {
      return false;
    }
  }

  const float DEFAULT_CEILING_TAPER = 64.0F;
  Data->CeilingTaper = DEFAULT_CEILING_TAPER;
  ReadFloat(Json, "ceilingTaper", &Data->CeilingTaper);
  if (!(Data->CeilingTaper >= 1.0F)) {
    LogError("SHAPE: %s ceilingTaper must be at least 1", FilePath);
    return false;
  }
  return true;
}

void LoadTerrainShape(const char *FilePath) {
  ShapeState *State = GetShapeState();
  cJSON *Json = ParseJsonFile(FilePath, "using the built-in shape");
  if (Json == NULL) {
    InstallDefaultShape(&State->Data);
    return;
  }

  // Parsed into a scratch copy so a rejected file never leaves a half-written
  // shape behind.
  static ShapeData SParsed;
  if (ParseShapeFile(Json, &SParsed, FilePath)) {
    State->Data = SParsed;
    LogInfo("SHAPE: %d splines, %d points loaded from %s",
            State->Data.SplineCount, State->Data.PointCount, FilePath);
  } else {
    LogError("SHAPE: %s rejected, using the built-in shape", FilePath);
    InstallDefaultShape(&State->Data);
  }
  cJSON_Delete(Json);
}

void InitTerrainShape(void) {
  ShapeState *State = GetShapeState();
  for (int Field = 0; Field < SHAPE_FIELD_COUNT; Field++) {
    State->Fields[Field] = NOISE_FIELD_INVALID;
  }
  InstallDefaultFields(State);
  DefineFields(State);
  InstallDefaultShape(&State->Data);
}

static float EvaluateSpline(const ShapeData *Data, int Index,
                            const ShapeCoords *Coords);

static float PointValue(const ShapeData *Data, const SplinePoint *Point,
                        const ShapeCoords *Coords) {
  return (Point->Child != SPLINE_NONE)
             ? EvaluateSpline(Data, Point->Child, Coords)
             : Point->Value;
}

// Cubic Hermite between neighbouring points; linear with the end derivative
// beyond either end.
static float EvaluateSpline(const ShapeData *Data, int Index,
                            const ShapeCoords *Coords) {
  const Spline *Curve = &Data->Splines[Index];
  const SplinePoint *Points = &Data->Points[Curve->FirstPoint];
  int Last = Curve->PointCount - 1;
  float Input = Coords->Values[Curve->Coordinate];

  if (Input <= Points[0].Location) {
    return PointValue(Data, &Points[0], Coords) +
           (Points[0].Derivative * (Input - Points[0].Location));
  }
  if (Input >= Points[Last].Location) {
    return PointValue(Data, &Points[Last], Coords) +
           (Points[Last].Derivative * (Input - Points[Last].Location));
  }

  int Segment = 0;
  while (Segment < Last - 1 && Input >= Points[Segment + 1].Location) {
    Segment++;
  }
  const SplinePoint *Left = &Points[Segment];
  const SplinePoint *Right = &Points[Segment + 1];

  // Cubic Hermite basis: h00 = 2t^3 - 3t^2 + 1, h10 = t^3 - 2t^2 + t,
  // h01 = 3t^2 - 2t^3, h11 = t^3 - t^2.
  const float TWO = 2.0F;
  const float THREE = 3.0F;
  float Span = Right->Location - Left->Location;
  float T = (Input - Left->Location) / Span;
  float T2 = T * T;
  float T3 = T2 * T;
  float H00 = (TWO * T3) - (THREE * T2) + 1.0F;
  float H10 = T3 - (TWO * T2) + T;
  float H01 = (-TWO * T3) + (THREE * T2);
  float H11 = T3 - T2;

  return (H00 * PointValue(Data, Left, Coords)) +
         (H10 * Span * Left->Derivative) +
         (H01 * PointValue(Data, Right, Coords)) +
         (H11 * Span * Right->Derivative);
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

ShapeCoords SampleShapeCoords(int GlobalX, int GlobalZ, uint64_t Seed) {
  const ShapeState *State = GetShapeState();
  float X = (float)GlobalX;
  float Z = (float)GlobalZ;
  float Weirdness =
      NoiseSample2D(State->Fields[SHAPE_FIELD_WEIRDNESS], Seed, X, Z);

  ShapeCoords Coords;
  Coords.Values[SHAPE_COORD_CONTINENTALNESS] =
      NoiseSample2D(State->Fields[SHAPE_FIELD_CONTINENTALNESS], Seed, X, Z);
  Coords.Values[SHAPE_COORD_EROSION] =
      NoiseSample2D(State->Fields[SHAPE_FIELD_EROSION], Seed, X, Z);
  Coords.Values[SHAPE_COORD_WEIRDNESS] = Weirdness;
  // Folds weirdness into ridges (PV = 1 at |W| = 2/3) and valleys (PV = -1 at
  // W = 0): PV = 1 - |3|W| - 2|.
  const float FOLD_SCALE = 3.0F;
  const float FOLD_OFFSET = 2.0F;
  Coords.Values[SHAPE_COORD_PEAKS_VALLEYS] =
      1.0F - fabsf((FOLD_SCALE * fabsf(Weirdness)) - FOLD_OFFSET);
  return Coords;
}

// Splines and the jagged term, before any landform modifier.
static ColumnShape SplineShape(int LatticeX, int LatticeZ, uint64_t Seed,
                               ShapeCoords *OutCoords) {
  const ShapeState *State = GetShapeState();
  const ShapeData *Data = &State->Data;
  ShapeCoords Coords = SampleShapeCoords(LatticeX, LatticeZ, Seed);
  *OutCoords = Coords;

  ColumnShape Shape;
  Shape.Base = EvaluateSpline(Data, Data->BaseRoot, &Coords);
  Shape.Variation = fmaxf(0.0F, EvaluateSpline(Data, Data->VariationRoot, &Coords));

  float Jaggedness = fmaxf(0.0F, EvaluateSpline(Data, Data->JaggedRoot, &Coords));
  if (Jaggedness > 0.0F) {
    float Jag = NoiseSample2D(State->Fields[SHAPE_FIELD_JAGGED], Seed,
                              (float)LatticeX, (float)LatticeZ);
    // Half-negative: dips are gentler than spikes, which reads as peaks rather
    // than noise.
    const float DIP_SCALE = 0.5F;
    Shape.Base += Jaggedness * ((Jag >= 0.0F) ? Jag : (Jag * DIP_SCALE));
  }
  Shape.ShellDepth = 0.0F;
  return Shape;
}

ColumnShape EvaluateColumnShapeUnmodified(int LatticeX, int LatticeZ,
                                          uint64_t Seed) {
  ShapeCoords Coords;
  ColumnShape Shape = SplineShape(LatticeX, LatticeZ, Seed, &Coords);
  Shape.ShellDepth = CaveShellDepth(LatticeX, LatticeZ, Shape.Base, Seed);
  return Shape;
}

// Per-thread memo of recent lattice columns. Neighbouring chunk columns share
// their edge lattice corners, and the structure pass probes the same columns
// repeatedly; the function is pure, so a hit returns exactly what a fresh
// evaluation would.
enum {
  SHAPE_MEMO_SLOTS = 512
};

typedef struct {
  int X;
  int Z;
  uint64_t Seed;
  bool Valid;
  ColumnShape Shape;
} ShapeMemoEntry;

static ColumnShape EvaluateColumnShapeFresh(int LatticeX, int LatticeZ,
                                            uint64_t Seed);

ColumnShape EvaluateColumnShape(int LatticeX, int LatticeZ, uint64_t Seed) {
  static _Thread_local ShapeMemoEntry SMemo[SHAPE_MEMO_SLOTS];
  const unsigned int HALF_BITS = 32U;
  uint64_t Key = ((uint64_t)(uint32_t)LatticeX << HALF_BITS) |
                 (uint64_t)(uint32_t)LatticeZ;
  ShapeMemoEntry *Entry =
      &SMemo[SplitMix64(Key ^ Seed) % (uint64_t)SHAPE_MEMO_SLOTS];
  if (Entry->Valid && Entry->X == LatticeX && Entry->Z == LatticeZ &&
      Entry->Seed == Seed) {
    return Entry->Shape;
  }
  ColumnShape Shape = EvaluateColumnShapeFresh(LatticeX, LatticeZ, Seed);
  *Entry = (ShapeMemoEntry){.X = LatticeX,
                            .Z = LatticeZ,
                            .Seed = Seed,
                            .Valid = true,
                            .Shape = Shape};
  return Shape;
}

static ColumnShape EvaluateColumnShapeFresh(int LatticeX, int LatticeZ,
                                            uint64_t Seed) {
  ShapeCoords Coords;
  ColumnShape Shape = SplineShape(LatticeX, LatticeZ, Seed, &Coords);
  // Landforms last, so the cave shell follows the modified surface.
  ApplyShapeModifiers(LatticeX, LatticeZ, &Shape.Base, &Shape.Variation, Seed);
  Shape.ShellDepth = CaveShellDepth(LatticeX, LatticeZ, Shape.Base, Seed);
  return Shape;
}

float GetCeilingTaper(void) { return GetShapeState()->Data.CeilingTaper; }

float SampleTerrainNoise3D(int X, int Y, int Z, uint64_t Seed) {
  const ShapeState *State = GetShapeState();
  return NoiseSample3D(State->Fields[SHAPE_FIELD_TERRAIN], Seed, (float)X,
                       (float)Y * State->TerrainVerticalScale, (float)Z);
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------

// Steepest the curve can be next to point Idx: a bound used only to size the
// continuity tolerance.
static float LocalSlopeBound(const SplinePoint *Points, int Count, int Idx) {
  const float HERMITE_SLOPE_FACTOR = 1.5F;
  float Bound = fabsf(Points[Idx].Derivative);
  for (int Side = -1; Side <= 1; Side += 2) {
    int Other = Idx + Side;
    if (Other < 0 || Other >= Count || Points[Other].Child != SPLINE_NONE) {
      continue;
    }
    float Span = fabsf(Points[Other].Location - Points[Idx].Location);
    float Rise = fabsf(Points[Other].Value - Points[Idx].Value);
    float Slope = (HERMITE_SLOPE_FACTOR * Rise / Span) +
                  fabsf(Points[Other].Derivative) + fabsf(Points[Idx].Derivative);
    Bound = fmaxf(Bound, Slope);
  }
  return Bound;
}

bool TerrainShapeSelfCheck(char *Failure, int FailureSize, int *OutChecked) {
  const float EPSILON = 1.0e-3F;
  const float SLACK = 1.0e-3F;
  const ShapeData *Data = &GetShapeState()->Data;
  int Checked = 0;

  for (int Index = 0; Index < Data->SplineCount; Index++) {
    const Spline *Curve = &Data->Splines[Index];
    const SplinePoint *Points = &Data->Points[Curve->FirstPoint];
    for (int Idx = 0; Idx < Curve->PointCount; Idx++) {
      if (Points[Idx].Child != SPLINE_NONE) {
        continue;
      }
      ShapeCoords Coords = {{0.0F, 0.0F, 0.0F, 0.0F}};
      float Location = Points[Idx].Location;
      Coords.Values[Curve->Coordinate] = Location;
      float At = EvaluateSpline(Data, Index, &Coords);
      if (At != Points[Idx].Value) {
        (void)snprintf(Failure, (size_t)FailureSize,
                       "spline %d point %d at %g gives %g, expected %g", Index,
                       Idx, (double)Location, (double)At,
                       (double)Points[Idx].Value);
        return false;
      }

      float Tolerance =
          (LocalSlopeBound(Points, Curve->PointCount, Idx) * EPSILON) + SLACK;
      for (int Side = -1; Side <= 1; Side += 2) {
        Coords.Values[Curve->Coordinate] = Location + ((float)Side * EPSILON);
        float Near = EvaluateSpline(Data, Index, &Coords);
        if (fabsf(Near - At) > Tolerance) {
          (void)snprintf(Failure, (size_t)FailureSize,
                         "spline %d is discontinuous at point %d (%g vs %g)",
                         Index, Idx, (double)Near, (double)At);
          return false;
        }
      }
      Checked++;
    }
  }
  *OutChecked = Checked;
  return true;
}

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

int GetPreliminaryFloor(int GlobalX, int GlobalZ, uint64_t Seed) {
  int X0 = FloorToLattice(GlobalX);
  int Z0 = FloorToLattice(GlobalZ);
  float Fx = (float)(GlobalX - X0) / (float)SHAPE_LATTICE_STEP;
  float Fz = (float)(GlobalZ - Z0) / (float)SHAPE_LATTICE_STEP;

  float Levels[2][2];
  for (int Ix = 0; Ix < 2; Ix++) {
    for (int Iz = 0; Iz < 2; Iz++) {
      ColumnShape Shape =
          EvaluateColumnShape(X0 + (Ix * SHAPE_LATTICE_STEP),
                              Z0 + (Iz * SHAPE_LATTICE_STEP), Seed);
      Levels[Ix][Iz] = ShapeFloorLevel(Shape.Base, Shape.Variation);
    }
  }
  return (int)floorf(ShapeBilinear(Levels[0][0], Levels[1][0], Levels[0][1],
                                   Levels[1][1], Fx, Fz));
}
