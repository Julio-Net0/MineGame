#include "render/lod_settings.h"

#include "core/launch_options.h"
#include "core/log.h"
#include "render/backend.h"
#include "render/lod_renderer.h"
#include "world/lod/lod_fill.h"
#include "world/lod/lod_ingest.h"
#include "world/lod/lod_persist.h"

#include <stdio.h>

#define BYTES_PER_MEGABYTE 1048576.0

enum { DISTANCE_TEXT_CHARS = 32 };

static LodSettings *GetSettings(void) {
  static LodSettings Settings;
  return &Settings;
}

const LodSettings *LodSettingsGet(void) { return GetSettings(); }

const char *LodSettingsUnavailableReason(void) {
  if (GetSettings()->Supported) {
    return NULL;
  }
  if (!RenderGetCapabilities()->LodSupported) {
    return GetLaunchOptions()->LodDisabled ? "turned off with --lod off"
                                           : "requires OpenGL 4.3";
  }
  return "could not be set up";
}

static void ApplyFog(const LodSettings *Settings) {
  RenderLodSetFog(Settings->FogOn, Settings->FogEnd);
}

static void ApplyDraw(const LodSettings *Settings) {
  LodRendererSetDrawOn(Settings->DrawOn && Settings->Distance > 0);
}

static void ApplyView(const LodSettings *Settings) {
  int Refine = LodQualityRefineFactor(Settings->Quality);
  LodRendererSetRefineFactor(Refine);
  LodFillSetView(Settings->Distance, Refine);
  LodIngestSetView(Settings->Distance, Refine);
}

void LodSettingsInit(void) {
  LodSettings *Settings = GetSettings();
  *Settings = (LodSettings){
      .Supported = LodRendererEnabled(),
      .DrawOn = true,
      .Distance = LaunchLodDistance(),
      .Quality = (LodQuality)LaunchLodQuality(),
      .FogOn = !GetLaunchOptions()->LodNoFog,
      .FogEnd = (float)LaunchLodFogEnd(),
  };
  if (!Settings->Supported) {
    return;
  }
  ApplyView(Settings);
  ApplyFog(Settings);
  ApplyDraw(Settings);
}

bool LodSettingsDistanceValid(int Chunks) {
  return Chunks == 0 ||
         (Chunks >= LOD_RUNTIME_DISTANCE_MIN && Chunks <= LOD_DISTANCE_MAX);
}

bool LodSettingsSetDistance(int Chunks) {
  LodSettings *Settings = GetSettings();
  if (!Settings->Supported || !LodSettingsDistanceValid(Chunks)) {
    return false;
  }
  if (Chunks == Settings->Distance) {
    return true;
  }
  Settings->Distance = Chunks;
  ApplyView(Settings);
  if (Chunks > 0 && !LaunchLodFogEndLocked()) {
    Settings->FogEnd = (float)(Chunks * LOD_BLOCKS_PER_CHUNK);
    ApplyFog(Settings);
  }
  ApplyDraw(Settings);
  LogInfo("LOD: distance %d chunks", Chunks);
  return true;
}

bool LodSettingsSetQuality(LodQuality Quality) {
  LodSettings *Settings = GetSettings();
  if (!Settings->Supported || Quality < LOD_QUALITY_LOW || Quality >= LOD_QUALITY_COUNT) {
    return false;
  }
  if (Quality != Settings->Quality) {
    Settings->Quality = Quality;
    ApplyView(Settings);
    LogInfo("LOD: quality %s (refine factor %d)", LodQualityName(Quality),
            LodQualityRefineFactor(Quality));
  }
  return true;
}

bool LodSettingsSetFog(bool On) {
  LodSettings *Settings = GetSettings();
  if (!Settings->Supported) {
    return false;
  }
  Settings->FogOn = On;
  ApplyFog(Settings);
  return true;
}

bool LodSettingsSetDrawOn(bool On) {
  LodSettings *Settings = GetSettings();
  if (!Settings->Supported) {
    return false;
  }
  Settings->DrawOn = On;
  ApplyDraw(Settings);
  return true;
}

void LodSettingsStatus(char *Settings, size_t SettingsSize, char *Usage,
                       size_t UsageSize) {
  const LodSettings *Current = GetSettings();
  const char *Reason = LodSettingsUnavailableReason();
  if (Reason != NULL) {
    snprintf(Settings, SettingsSize, "LOD unavailable: %s", Reason);
    Usage[0] = '\0';
    return;
  }
  char Distance[DISTANCE_TEXT_CHARS];
  if (Current->Distance > 0) {
    snprintf(Distance, sizeof(Distance), "%d chunks", Current->Distance);
  } else {
    snprintf(Distance, sizeof(Distance), "off");
  }
  snprintf(Settings, SettingsSize, "LOD %s, distance %s, quality %s, fog %s (%.0f)",
           LodRendererDrawOn() ? "drawn" : "hidden", Distance,
           LodQualityName(Current->Quality), Current->FogOn ? "on" : "off",
           (double)Current->FogEnd);
  LodRenderStats Draw;
  LodRendererGetStats(&Draw);
  LodPersistStats Cache;
  LodPersistGetStats(&Cache);
  int Written = snprintf(Usage, UsageSize, "%d sections %.1f MB, arena %.0f/%.0f MB",
                         LodIngestSectionCount(),
                         (double)LodIngestBytes() / BYTES_PER_MEGABYTE,
                         (double)Draw.Gpu.ArenaUsedBytes / BYTES_PER_MEGABYTE,
                         (double)Draw.Gpu.ArenaCapacityBytes / BYTES_PER_MEGABYTE);
  if (Written > 0 && (size_t)Written < UsageSize) {
    if (Cache.Enabled) {
      snprintf(Usage + Written, UsageSize - (size_t)Written,
               ", cache %llu hits %.1f MB",
               (unsigned long long)Cache.Hits,
               (double)Cache.Cache.DiskBytes / BYTES_PER_MEGABYTE);
    } else {
      snprintf(Usage + Written, UsageSize - (size_t)Written, ", no disk cache");
    }
  }
}
