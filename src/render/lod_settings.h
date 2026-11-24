#ifndef RENDER_LOD_SETTINGS_H
#define RENDER_LOD_SETTINGS_H

#include "world/lod/lod_quality.h"
#include <stdbool.h>
#include <stddef.h>

// The player-facing LOD settings and the one place that applies them at
// runtime (pause menu, /lod, scripts): the LOD distance reaches the synthetic
// fill, the store's clipmap radii and the fog end; the quality reaches the
// renderer's and the fill's refine factor; drawing and fog go to the renderer
// and the backend. Nothing is torn down or rebuilt: the store, its cache and
// the cached meshes stay, and each consumer picks the change up on its next
// update. Main thread only.

typedef struct {
  bool Supported;
  // Drawing as the player asked (/lod on|off); the LOD is drawn when this is
  // on and Distance is not 0.
  bool DrawOn;
  // Chunks, 0 when off.
  int Distance;
  LodQuality Quality;
  bool FogOn;
  // Blocks where the fog reaches the sky; follows Distance unless
  // --lod-fog-end fixed it.
  float FogEnd;
} LodSettings;

// After the LOD renderer and fill are set up (or failed to be).
void LodSettingsInit(void);

const LodSettings *LodSettingsGet(void);
// Why the LOD cannot be used on this run; NULL when it can.
const char *LodSettingsUnavailableReason(void);

// 0, or LOD_RUNTIME_DISTANCE_MIN..LOD_DISTANCE_MAX chunks.
bool LodSettingsDistanceValid(int Chunks);

// Each returns false (and changes nothing) when the LOD is unavailable or the
// value is not valid.
bool LodSettingsSetDistance(int Chunks);
bool LodSettingsSetQuality(LodQuality Quality);
bool LodSettingsSetFog(bool On);
bool LodSettingsSetDrawOn(bool On);

// Two short lines for the chat: the settings, then what they cost.
void LodSettingsStatus(char *Settings, size_t SettingsSize, char *Usage,
                       size_t UsageSize);

#endif
