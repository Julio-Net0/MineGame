#include "core/noise.h"

// Raylib vendors its own copy of stb_perlin for GenImagePerlinNoise and exports
// it from rtextures.c with external linkage. rtextures.c is pulled into every
// link for texture loading, so instantiating a second copy under the upstream
// names fails with `multiple definition of stb_perlin_fbm_noise3`.
//
// The usual fix, STB_PERLIN_STATIC, does not exist in this vendored copy — its
// functions are defined with external linkage unconditionally. So the names are
// rewritten before the implementation is included: the preprocessor renames each
// upstream entry point to an engine-private one, leaving no symbol that can
// collide with Raylib's. The vendored header stays byte-identical to upstream,
// which keeps it cheap to re-vendor later.
//
// This list must cover every externally-linked symbol the implementation
// defines. If the vendored copy is updated and gains a new entry point, the link
// will fail loudly with a multiple-definition error naming it — a missing rename
// cannot fail silently.
#define stb_perlin_noise3_internal MgPerlinNoise3Internal
#define stb_perlin_noise3 MgPerlinNoise3
#define stb_perlin_noise3_seed MgPerlinNoise3Seed
#define stb_perlin_noise3_wrap_nonpow2 MgPerlinNoise3WrapNonPow2
#define stb_perlin_ridge_noise3 MgPerlinRidgeNoise3
#define stb_perlin_fbm_noise3 MgPerlinFbmNoise3
#define stb_perlin_turbulence_noise3 MgPerlinTurbulenceNoise3

// This file is the only place the vendored noise header may be included.
#define STB_PERLIN_IMPLEMENTATION
#include "third_party/stb_perlin.h"

float NoiseFbm3(float X, float Y, float Z, float Lacunarity, float Gain,
                int Octaves) {
  return MgPerlinFbmNoise3(X, Y, Z, Lacunarity, Gain, Octaves);
}
