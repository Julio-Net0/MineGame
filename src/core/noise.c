#include "core/noise.h"
#include "core/hash.h"
#include <string.h>

// This file is the only place the vendored noise header may be included, so
// its fnl* implementation is compiled exactly once.
#define FNL_IMPL
#include "third_party/FastNoiseLite.h"

typedef struct {
  float X;
  float Y;
  float Z;
} DomainOffset;

typedef struct {
  fnl_state Template;
  uint64_t Salt;
  // The template already seeded and shifted for one world seed, so samples
  // with that seed skip the per-sample copy and hashing. Written only by
  // NoisePrepareSeed, before any worker thread exists.
  bool HasPrepared;
  uint64_t PreparedSeed;
  fnl_state Prepared;
  DomainOffset PreparedOffset;
} NoiseSlot;

typedef struct {
  NoiseSlot Slots[NOISE_MAX_FIELDS];
  int Count;
} NoiseTable;

static NoiseTable *GetNoiseTable(void) {
  static NoiseTable STable = {.Count = 0};
  return &STable;
}

static fnl_noise_type ToFnlKind(NoiseKind Kind) {
  switch (Kind) {
  case NOISE_KIND_OPENSIMPLEX2S:
    return FNL_NOISE_OPENSIMPLEX2S;
  case NOISE_KIND_PERLIN:
    return FNL_NOISE_PERLIN;
  case NOISE_KIND_VALUE:
    return FNL_NOISE_VALUE;
  case NOISE_KIND_CELLULAR:
    return FNL_NOISE_CELLULAR;
  case NOISE_KIND_OPENSIMPLEX2:
  default:
    return FNL_NOISE_OPENSIMPLEX2;
  }
}

static fnl_fractal_type ToFnlFractal(NoiseFractal Fractal) {
  switch (Fractal) {
  case NOISE_FRACTAL_FBM:
    return FNL_FRACTAL_FBM;
  case NOISE_FRACTAL_RIDGED:
    return FNL_FRACTAL_RIDGED;
  case NOISE_FRACTAL_PINGPONG:
    return FNL_FRACTAL_PINGPONG;
  case NOISE_FRACTAL_NONE:
  default:
    return FNL_FRACTAL_NONE;
  }
}

NoiseFieldDesc NoiseDefaultDesc(void) {
  const int DEFAULT_OCTAVES = 3;
  const float DEFAULT_FREQUENCY = 0.01F;
  const float DEFAULT_LACUNARITY = 2.0F;
  const float DEFAULT_GAIN = 0.5F;

  NoiseFieldDesc Desc = {.Kind = NOISE_KIND_OPENSIMPLEX2,
                         .Fractal = NOISE_FRACTAL_FBM,
                         .Octaves = DEFAULT_OCTAVES,
                         .Frequency = DEFAULT_FREQUENCY,
                         .Lacunarity = DEFAULT_LACUNARITY,
                         .Gain = DEFAULT_GAIN,
                         .ImproveXZ = false};
  return Desc;
}

bool NoiseFieldDefine(NoiseField *Field, const NoiseFieldDesc *Desc,
                      uint64_t Salt) {
  NoiseTable *Table = GetNoiseTable();

  int Index = *Field;
  if (Index < 0 || Index >= Table->Count) {
    if (Table->Count >= NOISE_MAX_FIELDS) {
      return false;
    }
    Index = Table->Count;
    Table->Count++;
  }

  fnl_state State = fnlCreateState();
  State.noise_type = ToFnlKind(Desc->Kind);
  State.fractal_type = ToFnlFractal(Desc->Fractal);
  State.octaves = (Desc->Octaves > 0) ? Desc->Octaves : 1;
  State.frequency = Desc->Frequency;
  State.lacunarity = Desc->Lacunarity;
  State.gain = Desc->Gain;
  State.rotation_type_3d =
      Desc->ImproveXZ ? FNL_ROTATION_IMPROVE_XZ_PLANES : FNL_ROTATION_NONE;

  Table->Slots[Index].Template = State;
  Table->Slots[Index].Salt = Salt;
  Table->Slots[Index].HasPrepared = false;
  *Field = Index;
  return true;
}

// Gradient noise is exactly zero on its lattice, so sampled unshifted every
// seed would put the same value at the world origin (and spawn at the same
// height). A per-field, per-seed shift of the sampling domain moves the origin
// off the lattice. Each component is a 24-bit value scaled to [-32768, 32768) in
// steps of 1/256: exactly representable in a float, small enough to cost no
// precision near the origin, and fractional so it never lands back on the
// lattice at a round frequency.
static float OffsetFromBits(uint64_t Bits) {
  const uint64_t MASK24 = 0xFFFFFFULL;
  const float STEPS_PER_BLOCK = 256.0F;
  const float HALF_RANGE = 32768.0F;
  return ((float)(Bits & MASK24) / STEPS_PER_BLOCK) - HALF_RANGE;
}

static DomainOffset OffsetFromHash(uint64_t Hash) {
  const unsigned int SHIFT_Y = 24U;
  const unsigned int SHIFT_Z = 48U;
  uint64_t Mixed = SplitMix64(Hash);
  // 64 bits hold only two full 24-bit fields; Z mixes once more.
  uint64_t MixedZ = SplitMix64(Mixed);
  DomainOffset Offset = {OffsetFromBits(Mixed), OffsetFromBits(Mixed >> SHIFT_Y),
                         OffsetFromBits(MixedZ >> SHIFT_Z)};
  return Offset;
}

// The template is copied so concurrent samplers never write shared state; only
// the copy carries the per-sample seed.
static bool PrepareState(NoiseField Field, uint64_t Seed, fnl_state *Out,
                         DomainOffset *Offset) {
  const NoiseTable *Table = GetNoiseTable();
  if (Field < 0 || Field >= Table->Count) {
    return false;
  }
  const NoiseSlot *Slot = &Table->Slots[Field];
  uint64_t Hash = SplitMix64(Seed ^ Slot->Salt);
  *Out = Slot->Template;
  Out->seed = (int)(uint32_t)Hash;
  *Offset = OffsetFromHash(Hash);
  return true;
}

void NoisePrepareSeed(uint64_t Seed) {
  NoiseTable *Table = GetNoiseTable();
  for (int Index = 0; Index < Table->Count; Index++) {
    NoiseSlot *Slot = &Table->Slots[Index];
    Slot->HasPrepared = PrepareState(Index, Seed, &Slot->Prepared,
                                     &Slot->PreparedOffset);
    Slot->PreparedSeed = Seed;
  }
}

// The prepared slot for Seed, or NULL when samples must prepare their own.
// FastNoiseLite never writes the state it samples, so threads share it.
static NoiseSlot *PreparedSlot(NoiseField Field, uint64_t Seed) {
  NoiseTable *Table = GetNoiseTable();
  if (Field < 0 || Field >= Table->Count) {
    return NULL;
  }
  NoiseSlot *Slot = &Table->Slots[Field];
  return (Slot->HasPrepared && Slot->PreparedSeed == Seed) ? Slot : NULL;
}

bool NoisePreparedMatches(int SamplesPerField) {
  NoiseTable *Table = GetNoiseTable();
  // Coprime wraps spread the samples over a 3D region rather than a line.
  const float STEP = 7.37F;
  const int WRAP_Y = 61;
  const int WRAP_Z = 97;
  for (int Index = 0; Index < Table->Count; Index++) {
    NoiseSlot *Slot = &Table->Slots[Index];
    if (!Slot->HasPrepared) {
      continue;
    }
    fnl_state Fresh;
    DomainOffset Offset;
    (void)PrepareState(Index, Slot->PreparedSeed, &Fresh, &Offset);
    for (int Sample = 0; Sample < SamplesPerField; Sample++) {
      float X = (float)Sample * STEP;
      float Y = (float)(Sample % WRAP_Y) * STEP;
      float Z = (float)(Sample % WRAP_Z) * -STEP;
      float Prepared = fnlGetNoise3D(&Slot->Prepared, X + Slot->PreparedOffset.X,
                                     Y + Slot->PreparedOffset.Y,
                                     Z + Slot->PreparedOffset.Z);
      float Reference =
          fnlGetNoise3D(&Fresh, X + Offset.X, Y + Offset.Y, Z + Offset.Z);
      if (Prepared != Reference) {
        return false;
      }
    }
  }
  return true;
}

float NoiseSample2D(NoiseField Field, uint64_t Seed, float X, float Z) {
  NoiseSlot *Slot = PreparedSlot(Field, Seed);
  if (Slot != NULL) {
    return fnlGetNoise2D(&Slot->Prepared, X + Slot->PreparedOffset.X,
                         Z + Slot->PreparedOffset.Z);
  }
  fnl_state State;
  DomainOffset Offset;
  if (!PrepareState(Field, Seed, &State, &Offset)) {
    return 0.0F;
  }
  return fnlGetNoise2D(&State, X + Offset.X, Z + Offset.Z);
}

float NoiseSample3D(NoiseField Field, uint64_t Seed, float X, float Y,
                    float Z) {
  NoiseSlot *Slot = PreparedSlot(Field, Seed);
  if (Slot != NULL) {
    return fnlGetNoise3D(&Slot->Prepared, X + Slot->PreparedOffset.X,
                         Y + Slot->PreparedOffset.Y, Z + Slot->PreparedOffset.Z);
  }
  fnl_state State;
  DomainOffset Offset;
  if (!PrepareState(Field, Seed, &State, &Offset)) {
    return 0.0F;
  }
  return fnlGetNoise3D(&State, X + Offset.X, Y + Offset.Y, Z + Offset.Z);
}

bool NoiseParseKind(const char *Name, NoiseKind *Out) {
  static const struct {
    const char *Name;
    NoiseKind Kind;
  } KINDS[] = {{"opensimplex2", NOISE_KIND_OPENSIMPLEX2},
               {"opensimplex2s", NOISE_KIND_OPENSIMPLEX2S},
               {"perlin", NOISE_KIND_PERLIN},
               {"value", NOISE_KIND_VALUE},
               {"cellular", NOISE_KIND_CELLULAR}};

  for (size_t Idx = 0; Idx < sizeof(KINDS) / sizeof(KINDS[0]); Idx++) {
    if (strcmp(Name, KINDS[Idx].Name) == 0) {
      *Out = KINDS[Idx].Kind;
      return true;
    }
  }
  return false;
}

bool NoiseParseFractal(const char *Name, NoiseFractal *Out) {
  static const struct {
    const char *Name;
    NoiseFractal Fractal;
  } FRACTALS[] = {{"none", NOISE_FRACTAL_NONE},
                  {"fbm", NOISE_FRACTAL_FBM},
                  {"ridged", NOISE_FRACTAL_RIDGED},
                  {"pingpong", NOISE_FRACTAL_PINGPONG}};

  for (size_t Idx = 0; Idx < sizeof(FRACTALS) / sizeof(FRACTALS[0]); Idx++) {
    if (strcmp(Name, FRACTALS[Idx].Name) == 0) {
      *Out = FRACTALS[Idx].Fractal;
      return true;
    }
  }
  return false;
}
