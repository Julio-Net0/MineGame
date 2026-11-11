#include "world/surface.h"
#include "core/fileio.h"
#include "core/log.h"
#include "third_party/cJSON.h"
#include "world/chunk.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#define SURFACE_FIELD_SALT 0x5E7FACE000000001ULL

#define DEFAULT_SURFACE_FREQUENCY 0.01F
#define DEFAULT_BASE_HEIGHT 6.0F
#define DEFAULT_AMPLITUDE 20.0F

enum {
  DEFAULT_SURFACE_OCTAVES = 4,
  SPAWN_SEARCH_RADIUS = 256,
  SPAWN_DROP_HEIGHT = 3
};

typedef struct {
  SurfaceParams Params;
  NoiseField Field;
} SurfaceState;

static SurfaceState *GetSurfaceState(void) {
  static SurfaceState SState = {.Field = NOISE_FIELD_INVALID};
  return &SState;
}

static void DefineSurfaceField(SurfaceState *State) {
  if (!NoiseFieldDefine(&State->Field, &State->Params.Field,
                        SURFACE_FIELD_SALT)) {
    LogError("SURFACE: noise field table is full; terrain will be flat");
  }
}

void InitSurfaceParams(void) {
  SurfaceState *State = GetSurfaceState();
  State->Params.Field = NoiseDefaultDesc();
  State->Params.Field.Octaves = DEFAULT_SURFACE_OCTAVES;
  State->Params.Field.Frequency = DEFAULT_SURFACE_FREQUENCY;
  State->Params.BaseHeight = DEFAULT_BASE_HEIGHT;
  State->Params.Amplitude = DEFAULT_AMPLITUDE;
  DefineSurfaceField(State);
}

static void ReadFloat(const cJSON *Object, const char *Key, float *Out) {
  const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Object, Key);
  if (cJSON_IsNumber(Item) != 0) {
    *Out = (float)Item->valuedouble;
  }
}

static void ReadFieldDesc(const cJSON *Object, NoiseFieldDesc *Desc,
                          const char *FilePath) {
  const cJSON *KindItem = cJSON_GetObjectItemCaseSensitive(Object, "kind");
  if (cJSON_IsString(KindItem) != 0 &&
      !NoiseParseKind(KindItem->valuestring, &Desc->Kind)) {
    LogError("SURFACE: unknown noise kind '%s' in %s, keeping default",
             KindItem->valuestring, FilePath);
  }

  const cJSON *FractalItem = cJSON_GetObjectItemCaseSensitive(Object, "fractal");
  if (cJSON_IsString(FractalItem) != 0 &&
      !NoiseParseFractal(FractalItem->valuestring, &Desc->Fractal)) {
    LogError("SURFACE: unknown fractal '%s' in %s, keeping default",
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

void LoadSurfaceParams(const char *FilePath) {
  SurfaceState *State = GetSurfaceState();

  char *FileContent = ReadTextFile(FilePath);
  if (FileContent == NULL) {
    LogWarn("SURFACE: no params at %s, keeping built-in defaults", FilePath);
    return;
  }

  cJSON *Json = cJSON_Parse(FileContent);
  if (Json == NULL) {
    LogError("SURFACE: JSON syntax error in %s before: %s, keeping defaults",
             FilePath, cJSON_GetErrorPtr());
    free(FileContent);
    return;
  }

  const cJSON *SurfaceItem = cJSON_GetObjectItemCaseSensitive(Json, "surface");
  if (cJSON_IsObject(SurfaceItem) != 0) {
    ReadFieldDesc(SurfaceItem, &State->Params.Field, FilePath);
    ReadFloat(SurfaceItem, "baseHeight", &State->Params.BaseHeight);
    ReadFloat(SurfaceItem, "amplitude", &State->Params.Amplitude);
  } else {
    LogWarn("SURFACE: %s has no \"surface\" object, keeping defaults",
            FilePath);
  }

  DefineSurfaceField(State);

  LogInfo("SURFACE: octaves=%d frequency=%.4f base=%.1f amplitude=%.1f",
          State->Params.Field.Octaves, (double)State->Params.Field.Frequency,
          (double)State->Params.BaseHeight, (double)State->Params.Amplitude);

  cJSON_Delete(Json);
  free(FileContent);
}

const SurfaceParams *GetSurfaceParams(void) {
  return &GetSurfaceState()->Params;
}

int GetPreliminarySurface(int GlobalX, int GlobalZ, uint64_t Seed) {
  const SurfaceState *State = GetSurfaceState();
  float NoiseVal =
      NoiseSample2D(State->Field, Seed, (float)GlobalX, (float)GlobalZ);
  return (int)floorf(State->Params.BaseHeight +
                     (NoiseVal * State->Params.Amplitude));
}

// Walks ring R of the square spiral in a fixed order (top edge, right, bottom,
// left) and reports the first column above sea level.
static bool SearchRing(int Ring, uint64_t Seed, SpawnPoint *Out) {
  int Side = 2 * Ring;
  int PosX = -Ring;
  int PosZ = -Ring;
  const int STEP_X[4] = {1, 0, -1, 0};
  const int STEP_Z[4] = {0, 1, 0, -1};

  for (int Edge = 0; Edge < 4; Edge++) {
    for (int Step = 0; Step < Side; Step++) {
      int Surface = GetPreliminarySurface(PosX, PosZ, Seed);
      if (Surface > SEA_LEVEL) {
        Out->X = PosX;
        Out->Z = PosZ;
        Out->Surface = Surface;
        return true;
      }
      PosX += STEP_X[Edge];
      PosZ += STEP_Z[Edge];
    }
  }
  return false;
}

static SpawnPoint SearchSpawnColumn(uint64_t Seed) {
  SpawnPoint Spawn = {.X = 0, .Z = 0,
                      .Surface = GetPreliminarySurface(0, 0, Seed)};

  if (Spawn.Surface <= SEA_LEVEL) {
    bool Found = false;
    for (int Ring = 1; Ring <= SPAWN_SEARCH_RADIUS && !Found; Ring++) {
      Found = SearchRing(Ring, Seed, &Spawn);
    }
    if (!Found) {
      Spawn.X = 0;
      Spawn.Z = 0;
      Spawn.Surface = GetPreliminarySurface(0, 0, Seed);
    }
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
