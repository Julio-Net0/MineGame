#ifndef WORLD_LOD_LOD_QUALITY_H
#define WORLD_LOD_LOD_QUALITY_H

#include <stdbool.h>
#include <string.h>

// How fine the LOD is drawn: a section is refined into its children while the
// camera is within RefineFactor of its widths, so each step doubles or halves
// the screen size a voxel reaches before the next level takes over. Medium is
// the factor the LOD was built with. The renderer and the synthetic fill use
// the same factor, so every column the renderer wants is generated.
typedef enum {
  LOD_QUALITY_LOW = 0,
  LOD_QUALITY_MEDIUM,
  LOD_QUALITY_HIGH,
  LOD_QUALITY_COUNT
} LodQuality;

enum { LOD_QUALITY_BASE_REFINE = 4 };

static inline int LodQualityRefineFactor(LodQuality Quality) {
  return LOD_QUALITY_BASE_REFINE << (unsigned)Quality;
}

static inline const char *LodQualityName(LodQuality Quality) {
  switch (Quality) {
  case LOD_QUALITY_LOW:
    return "low";
  case LOD_QUALITY_HIGH:
    return "high";
  case LOD_QUALITY_MEDIUM:
  case LOD_QUALITY_COUNT:
  default:
    return "medium";
  }
}

static inline bool LodQualityParse(const char *Text, LodQuality *Out) {
  for (int Idx = 0; Idx < (int)LOD_QUALITY_COUNT; Idx++) {
    if (strcmp(Text, LodQualityName((LodQuality)Idx)) == 0) {
      *Out = (LodQuality)Idx;
      return true;
    }
  }
  return false;
}

#endif
