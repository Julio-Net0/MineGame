#ifndef WORLD_WORLDGEN_STRATA_H
#define WORLD_WORLDGEN_STRATA_H

#include <stdbool.h>

// Vertical layout of the world. Every Y belongs to exactly one stratum, and a
// stratum names one of the generators compiled into the engine. Data chooses the
// ranges (assets/worldgen/strata.json); it can never introduce generator code.
// A future generator (a Nether below the bedrock floor, floatlands above the
// overworld) is a new enum value plus a JSON entry, not a rewrite of terrain
// generation.
//
// Written only during startup, before the chunk workers exist; read-only and
// lock-free afterwards.

typedef enum {
  STRATUM_GEN_OVERWORLD = 0,
  STRATUM_GEN_BEDROCK_BAND,
  STRATUM_GEN_VOID,
  STRATUM_GEN_COUNT
} StratumGenerator;

enum {
  MAX_STRATA = 8,
  MAX_STRATUM_NAME = 32
};

typedef struct {
  char Name[MAX_STRATUM_NAME];
  StratumGenerator Generator;
  // Inclusive; INT_MIN / INT_MAX mark a bound open to infinity.
  int MinY;
  int MaxY;
} Stratum;

// Installs the built-in default table and generation ceiling.
void InitStrata(void);

// Replaces the table from JSON when it is valid; otherwise logs why and keeps
// the built-in default.
void LoadStrata(const char *FilePath);

StratumGenerator ResolveStratum(int GlobalY);

// Lowest Y whose cubes are generated as all air. Building above it is allowed.
int GetGenerationCeiling(void);

// Inclusive Y range of the first stratum using Generator; false if none does.
bool GetStratumRange(StratumGenerator Generator, int *MinY, int *MaxY);

#endif
