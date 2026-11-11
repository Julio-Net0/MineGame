#ifndef WORLD_CLIMATE_H
#define WORLD_CLIMATE_H

#include "core/noise.h"
#include <stdint.h>

// Temperature and humidity: salted 2D fields over (X, Z) mapped from [-1, 1] to
// [0, 1], temperature lowered with height above LapseStart. The one definition
// shared by biome selection and the terrain shape modifiers. Configured during
// startup only; read-only and lock-free afterwards.

void ClimateConfigure(const NoiseFieldDesc *Temperature,
                      const NoiseFieldDesc *Humidity, float Lapse,
                      float LapseStart);

// Temperature at (X, Z) for ground at Height, clamped to [0, 1].
float ClimateTemperature(int X, int Z, float Height, uint64_t Seed);

// Humidity at (X, Z), clamped to [0, 1].
float ClimateHumidity(int X, int Z, uint64_t Seed);

#endif
