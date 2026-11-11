#include "world/climate.h"
#include "core/log.h"
#include <math.h>

// Each axis is its own salted field, so temperature, humidity, and the terrain
// fields never sample correlated noise.
#define TEMPERATURE_FIELD_SALT 0xC11A7E0000000001ULL
#define HUMIDITY_FIELD_SALT 0xC11A7E0000000002ULL

typedef struct {
  NoiseField Temperature;
  NoiseField Humidity;
  float Lapse;
  float LapseStart;
} ClimateState;

static ClimateState *GetClimateState(void) {
  static ClimateState SState = {.Temperature = NOISE_FIELD_INVALID,
                                .Humidity = NOISE_FIELD_INVALID,
                                .Lapse = 0.0F,
                                .LapseStart = 0.0F};
  return &SState;
}

void ClimateConfigure(const NoiseFieldDesc *Temperature,
                      const NoiseFieldDesc *Humidity, float Lapse,
                      float LapseStart) {
  ClimateState *State = GetClimateState();
  if (!NoiseFieldDefine(&State->Temperature, Temperature,
                        TEMPERATURE_FIELD_SALT) ||
      !NoiseFieldDefine(&State->Humidity, Humidity, HUMIDITY_FIELD_SALT)) {
    LogError("CLIMATE: noise field table is full; a climate axis is flat");
  }
  State->Lapse = Lapse;
  State->LapseStart = LapseStart;
}

static float ClampUnit(float Value) {
  return fminf(1.0F, fmaxf(0.0F, Value));
}

static float SampleUnitField(NoiseField Field, int X, int Z, uint64_t Seed) {
  const float MIDPOINT = 0.5F;
  float Raw = NoiseSample2D(Field, Seed, (float)X, (float)Z);
  return (Raw + 1.0F) * MIDPOINT;
}

float ClimateTemperature(int X, int Z, float Height, uint64_t Seed) {
  const ClimateState *State = GetClimateState();
  float Altitude = fmaxf(0.0F, Height - State->LapseStart);
  float Temperature = SampleUnitField(State->Temperature, X, Z, Seed) -
                      (State->Lapse * Altitude);
  return ClampUnit(Temperature);
}

float ClimateHumidity(int X, int Z, uint64_t Seed) {
  return ClampUnit(SampleUnitField(GetClimateState()->Humidity, X, Z, Seed));
}
