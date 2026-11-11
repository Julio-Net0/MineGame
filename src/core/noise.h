#ifndef CORE_NOISE_H
#define CORE_NOISE_H

#include <stdbool.h>
#include <stdint.h>

// Renderer-free noise facility owned by the engine.
//
// The vendored FastNoiseLite implementation is instantiated only inside
// noise.c, and no library type crosses this header. Callers describe a field
// with engine-owned types and sample it through a handle, which is what lets the
// backend be replaced (for example by a SIMD library) without touching world
// code.
//
// A field stores its parameters and a salt, never a seed. Every sample takes the
// world seed and derives, from SplitMix64(Seed ^ Salt), both the library seed
// and a fractional shift of the sampling domain, so the samplers built on top
// stay pure functions of their seed argument, fields can be defined before the
// world seed is known, and the world origin is not a lattice point (where
// gradient noise is zero for every seed).
//
// Fields are defined during startup, before the chunk workers exist, and are
// read-only afterwards: concurrent sampling takes no lock.

typedef enum {
  NOISE_KIND_OPENSIMPLEX2 = 0,
  NOISE_KIND_OPENSIMPLEX2S,
  NOISE_KIND_PERLIN,
  NOISE_KIND_VALUE,
  NOISE_KIND_CELLULAR
} NoiseKind;

typedef enum {
  NOISE_FRACTAL_NONE = 0,
  NOISE_FRACTAL_FBM,
  NOISE_FRACTAL_RIDGED,
  NOISE_FRACTAL_PINGPONG
} NoiseFractal;

typedef struct {
  NoiseKind Kind;
  NoiseFractal Fractal;
  int Octaves;
  float Frequency;
  float Lacunarity;
  float Gain;
  // Rotates 3D sampling so XZ planes show no lattice alignment; meant for
  // Y-up voxel terrain. Ignored by 2D sampling.
  bool ImproveXZ;
} NoiseFieldDesc;

typedef int NoiseField;

enum {
  NOISE_FIELD_INVALID = -1,
  NOISE_MAX_FIELDS = 32
};

// Defines *Field from Desc and Salt. A field still NOISE_FIELD_INVALID is
// created; an existing one is redefined in place, so repeated initialization
// never exhausts the table. Returns false when the table is full.
bool NoiseFieldDefine(NoiseField *Field, const NoiseFieldDesc *Desc,
                      uint64_t Salt);

// Prepares every defined field for one world seed: samples with that seed then
// use a ready state instead of seeding a copy per sample. Values are identical
// either way; other seeds keep working through the per-sample path. Call after
// all fields are defined and before any thread samples (redefining a field
// drops its preparation).
void NoisePrepareSeed(uint64_t Seed);

// Self-test: every prepared field samples exactly like a field prepared afresh
// for the same seed.
bool NoisePreparedMatches(int SamplesPerField);

// Fractal output is normalized by the library, roughly within [-1, 1] for any
// octave count. An invalid handle samples as 0.
float NoiseSample2D(NoiseField Field, uint64_t Seed, float X, float Z);
float NoiseSample3D(NoiseField Field, uint64_t Seed, float X, float Y, float Z);

// Parse the lower-case names used by data files ("opensimplex2", "fbm", ...).
// Return false and leave *Out untouched for an unknown name.
bool NoiseParseKind(const char *Name, NoiseKind *Out);
bool NoiseParseFractal(const char *Name, NoiseFractal *Out);

// Defaults shared by every data-driven field: OpenSimplex2 fBm.
NoiseFieldDesc NoiseDefaultDesc(void);

#endif
