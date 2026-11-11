#ifndef AUTOMATION_WORLDGEN_PREVIEW_H
#define AUTOMATION_WORLDGEN_PREVIEW_H

#include <stdbool.h>

enum {
  WORLDGEN_PREVIEW_MIN_SIZE = 1,
  WORLDGEN_PREVIEW_MAX_SIZE = 1024,
  WORLDGEN_PREVIEW_ERROR_CHARS = 256
};

// Renders the terrain around (CenterX, CenterZ) to images a reviewer or a test
// can check without walking the world:
//   <Name>_height.png   Size x Size preliminary surface
//   <Name>_biome.png    Size x Size surface biome
//   <Name>_slice.png    Size wide, one row per Y from the bedrock floor to the
//                       generation ceiling, the X-Y slice at Z = CenterZ
//   <Name>_worldgen.json statistics (surface range, cube classes, timing,
//                       bedrock floor and ceiling checks)
//
// The slice runs the real generator into scratch chunks that never reach the
// world, so it shows exactly what the game generates. Runs synchronously on the
// calling thread. On failure returns false with a reason in Error.
bool WorldgenPreviewWrite(const char *OutDir, const char *Name, int CenterX,
                          int CenterZ, int Size, char *Error, int ErrorSize);

// A large, low-resolution surface biome map from the pure samplers only:
//   <Name>_biomes.png   (Size / Step) pixels square, one sample per Step blocks
//   <Name>_biomes.json  per-biome coverage (all cells and land cells), the
//                       number of distinct biomes, and how often ocean-family
//                       cells lie below sea level and peak-family cells lie
//                       above sea level + 64
// Size must be a positive multiple of Step with Size / Step <= 1024.
bool WorldgenBiomeMapWrite(const char *OutDir, const char *Name, int CenterX,
                           int CenterZ, int Size, int Step, char *Error,
                           int ErrorSize);

#endif
