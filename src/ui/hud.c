#include "ui/hud.h"
#include "player/player.h"
#include "core/color.h"
#include "platform/platform.h"
#include "render/renderer.h"
#include "render/backend.h"
#include "ui/block_icon.h"
#include "ui/font.h"
#include "world/world.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "world/worldgen_column_cache.h"
#include "persistence/world_save.h"
#include "core/launch_options.h"
#include "core/profiler.h"
#include <math.h>
#include <stdio.h>

// Hotbar and crosshair sizes are vanilla's, in GUI units: the sprites are
// authored at exactly these sizes, and a higher-resolution resource pack keeps
// them because the size comes from here rather than from the image.
enum {
  HOTBAR_WIDTH = 182,
  HOTBAR_HEIGHT = 22,
  HOTBAR_SLOT_PITCH = 20,
  HOTBAR_SELECTION_WIDTH = 24,
  HOTBAR_SELECTION_HEIGHT = 23,
  HOTBAR_ICON_INSET = 3,
  HOTBAR_ICON_SIZE = 16,
  CROSSHAIR_SIZE = 15,
  DEBUG_TEXT_CAPACITY = 96,
  // Vanilla F3 layout: lines start two units in from the corner, one font line
  // apart, each on its own translucent grey strip.
  DEBUG_MARGIN = 2,
  DEBUG_STRIP_PAD = 1,
  // Timing table columns. The font is proportional, so columns are placed
  // explicitly and numbers right-aligned instead of padded with spaces.
  DEBUG_NAME_COLUMN = 64,
  DEBUG_NUMBER_COLUMN = 34,
  DEBUG_NUMBER_COLUMNS = 4,
  DEBUG_SUFFIX_GAP = 4
};

typedef struct {
  UiSpriteId Hotbar;
  UiSpriteId HotbarSelection;
  UiSpriteId Crosshair;
} HudSprites;

static HudSprites *GetHudSprites(void) {
  static HudSprites Sprites;
  return &Sprites;
}

void InitHud(void) {
  HudSprites *Sprites = GetHudSprites();
  Sprites->Hotbar = GuiAtlasRegister("hud/hotbar");
  Sprites->HotbarSelection = GuiAtlasRegister("hud/hotbar_selection");
  Sprites->Crosshair = GuiAtlasRegister("hud/crosshair");
}

typedef struct {
  BlockID LastBlockId;
  int LastSelectedSlot;
  double ToastStartTime;
} HudToastState;

static HudToastState *GetHudToastState(void) {
  static HudToastState State = {.LastBlockId = 0,
                                .LastSelectedSlot = -1,
                                .ToastStartTime = 0.0};
  return &State;
}

static void DrawItemNameToast(UiContext *Ctx, Player *PlayerVal) {
  HudToastState *Toast = GetHudToastState();
  int CurrentSlot = PlayerVal->SelectedHotbarSlot;
  BlockID CurrentBlock = (CurrentSlot >= 0 && CurrentSlot < HOTBAR_SIZE)
                             ? PlayerVal->Hotbar[CurrentSlot]
                             : 0;

  if (CurrentSlot != Toast->LastSelectedSlot ||
      CurrentBlock != Toast->LastBlockId) {
    Toast->LastSelectedSlot = CurrentSlot;
    Toast->LastBlockId = CurrentBlock;
    if (CurrentBlock != 0) {
      Toast->ToastStartTime = PlatformGetTime();
    } else {
      Toast->ToastStartTime = 0.0;
    }
  }

  const double TOAST_SOLID_TIME = 1.5;
  const double TOAST_FADE_TIME = 0.5;
  const double TOAST_TOTAL_TIME = TOAST_SOLID_TIME + TOAST_FADE_TIME;

  if (Toast->LastBlockId == 0 || Toast->ToastStartTime <= 0.0) {
    return;
  }

  double Elapsed = PlatformGetTime() - Toast->ToastStartTime;
  if (Elapsed < 0.0 || Elapsed >= TOAST_TOTAL_TIME) {
    return;
  }

  float Alpha = 1.0F;
  if (Elapsed > TOAST_SOLID_TIME) {
    Alpha = (float)(1.0 - ((Elapsed - TOAST_SOLID_TIME) / TOAST_FADE_TIME));
    if (Alpha < 0.0F) {
      Alpha = 0.0F;
    }
  }

  const BlockType *Def = GetBlockDef(Toast->LastBlockId);
  if (Def == (void *)0 || Def->DisplayName[0] == '\0') {
    return;
  }

  const char *Name = Def->DisplayName;
  float TextWidth = UiMeasureText(Name);
  float TextX = ((float)Ctx->GuiWidth - TextWidth) * UI_HALF;
  const float TOAST_OFFSET_Y = 16.0F;
  float TextY = (float)(Ctx->GuiHeight - HOTBAR_HEIGHT) - TOAST_OFFSET_Y;

  const float ALPHA_SCALE = 255.0F;
  unsigned char AlphaByte = (unsigned char)(Alpha * ALPHA_SCALE);
  Color8 TextColor = (Color8){COLOR_WHITE.R, COLOR_WHITE.G, COLOR_WHITE.B, AlphaByte};
  UiTextShadow(Ctx, Name, TextX, TextY, TextColor);
}

static void DrawHotbar(UiContext *Ctx, Player *PlayerVal) {
  DrawItemNameToast(Ctx, PlayerVal);
  HudSprites *Sprites = GetHudSprites();
  int StartX = (Ctx->GuiWidth - HOTBAR_WIDTH) / 2;
  int StartY = Ctx->GuiHeight - HOTBAR_HEIGHT;

  UiSprite(Ctx, Sprites->Hotbar,
           (UiRect){StartX, StartY, HOTBAR_WIDTH, HOTBAR_HEIGHT}, COLOR_WHITE);

  // Offset one unit up and left: the selection frame is two units larger than
  // a slot so it overlaps the hotbar's own border.
  int Selected = PlayerVal->SelectedHotbarSlot;
  UiSprite(Ctx, Sprites->HotbarSelection,
           (UiRect){StartX - 1 + (Selected * HOTBAR_SLOT_PITCH), StartY - 1,
                    HOTBAR_SELECTION_WIDTH, HOTBAR_SELECTION_HEIGHT},
           COLOR_WHITE);

  for (int Idx = 0; Idx < HOTBAR_SIZE; Idx++) {
    BlockID BlockId = PlayerVal->Hotbar[Idx];
    if (BlockId != 0) {
      float IconX =
          (float)(StartX + HOTBAR_ICON_INSET + (Idx * HOTBAR_SLOT_PITCH));
      float IconY = (float)(StartY + HOTBAR_ICON_INSET);
      UiBlockIcon(Ctx, BlockId, IconX, IconY, (float)HOTBAR_ICON_SIZE);
    }
  }
}

static void DrawCrosshair(UiContext *Ctx) {
  int X = (Ctx->GuiWidth - CROSSHAIR_SIZE) / 2;
  int Y = (Ctx->GuiHeight - CROSSHAIR_SIZE) / 2;
  UiSprite(Ctx, GetHudSprites()->Crosshair,
           (UiRect){X, Y, CROSSHAIR_SIZE, CROSSHAIR_SIZE}, COLOR_WHITE);
}

#define DEBUG_TEXT_COLOR ((Color8){224, 224, 224, 255})
#define DEBUG_STRIP_COLOR ((Color8){80, 80, 80, 144})

static void DrawDebugStrip(UiContext *Ctx, int X, int Y, float Width) {
  UiFillRect(Ctx,
             (UiRect){X - DEBUG_STRIP_PAD, Y - DEBUG_STRIP_PAD,
                      (int)Width + (DEBUG_STRIP_PAD * 2), FONT_LINE_HEIGHT},
             DEBUG_STRIP_COLOR);
}

// One overlay line on its strip; returns the Y of the next line.
static int DrawDebugLine(UiContext *Ctx, const char *Text, int Y) {
  DrawDebugStrip(Ctx, DEBUG_MARGIN, Y, UiMeasureText(Text));
  UiText(Ctx, Text, (float)DEBUG_MARGIN, (float)Y, DEBUG_TEXT_COLOR);
  return Y + FONT_LINE_HEIGHT;
}

#ifdef MINEGAME_PROFILE

// Per-subsystem cost in microseconds, not milliseconds: on a fast machine the
// whole frame costs a fraction of a millisecond and every subsystem would read
// as zero. Average alongside peak and 1% low, because the stall this panel
// exists to diagnose happens on a minority of frames and an average hides it
// exactly when it matters.
//
// Recomputed a few times a second rather than every frame. Scanning the window
// per frame would waste work on a number nobody can read at 60 Hz, and a
// readout that changes 60 times a second is unreadable anyway.
#define TIMING_REFRESH_INTERVAL 0.25

typedef struct {
  ProfileStats Stats[PROFILE_SCOPE_COUNT];
  ProfileStats FrameStats;
  uint64_t SolidSamples;
  float SolidOverdraw;
  double LastRefreshTime;
  bool HasSamples;
  bool HasSolidSamples;
} TimingPanelState;

static TimingPanelState *GetTimingPanelState(void) {
  static TimingPanelState State;
  return &State;
}

// Name, up to four right-aligned numeric cells and a suffix, on one strip.
static int DrawTimingRow(UiContext *Ctx, int Y, const char *Name,
                         const char *const Cells[DEBUG_NUMBER_COLUMNS],
                         const char *Suffix) {
  float Width = (float)(DEBUG_NAME_COLUMN +
                        (DEBUG_NUMBER_COLUMNS * DEBUG_NUMBER_COLUMN));
  float SuffixWidth = UiMeasureText(Suffix);
  if (SuffixWidth > 0.0F) {
    Width += (float)DEBUG_SUFFIX_GAP + SuffixWidth;
  }
  DrawDebugStrip(Ctx, DEBUG_MARGIN, Y, Width);

  UiText(Ctx, Name, (float)DEBUG_MARGIN, (float)Y, DEBUG_TEXT_COLOR);
  float Right = (float)(DEBUG_MARGIN + DEBUG_NAME_COLUMN);
  for (int Col = 0; Col < DEBUG_NUMBER_COLUMNS; Col++) {
    Right += (float)DEBUG_NUMBER_COLUMN;
    if (Cells[Col] != NULL) {
      UiText(Ctx, Cells[Col], Right - UiMeasureText(Cells[Col]), (float)Y,
             DEBUG_TEXT_COLOR);
    }
  }
  if (SuffixWidth > 0.0F) {
    UiText(Ctx, Suffix, Right + (float)DEBUG_SUFFIX_GAP, (float)Y,
           DEBUG_TEXT_COLOR);
  }
  return Y + FONT_LINE_HEIGHT;
}

// Counts, not timings: how often generation short-circuited a cube and how often
// a cube found its column's 2D data already cached. Read next to "Worker gen"
// they explain a change in its cost.
static int DrawWorldgenCountersLine(UiContext *Ctx, int Y) {
  const double PERCENT = 100.0;
  uint64_t Classes[CUBE_CLASS_COUNT] = {0};
  GetCubeClassCounts(Classes);
  uint64_t Hits = 0;
  uint64_t Misses = 0;
  ColumnCacheGetTotals(&Hits, &Misses);
  uint64_t Lookups = Hits + Misses;
  double HitRate =
      (Lookups > 0) ? (PERCENT * (double)Hits / (double)Lookups) : 0.0;

  char Line[DEBUG_TEXT_CAPACITY];
  snprintf(Line, sizeof(Line),
           "Gen cubes: air %llu solid %llu mixed %llu | column cache %.1f%%",
           (unsigned long long)Classes[CUBE_CLASS_AIR],
           (unsigned long long)Classes[CUBE_CLASS_SOLID],
           (unsigned long long)Classes[CUBE_CLASS_MIXED], HitRate);
  return DrawDebugLine(Ctx, Line, Y);
}

static int DrawTimingPanel(UiContext *Ctx, int Y) {
  TimingPanelState *Panel = GetTimingPanelState();

  double Now = PlatformGetTime();
  if (!Panel->HasSamples || !Panel->HasSolidSamples ||
      (Now - Panel->LastRefreshTime) >= TIMING_REFRESH_INTERVAL) {
    for (int Scope = 0; Scope < PROFILE_SCOPE_COUNT; Scope++) {
      ProfilerQuery((ProfileScope)Scope, &Panel->Stats[Scope]);
    }
    ProfilerQueryFrame(&Panel->FrameStats);
    Panel->HasSolidSamples =
        RenderQuerySolidSamples(&Panel->SolidSamples, &Panel->SolidOverdraw);
    Panel->LastRefreshTime = Now;
    Panel->HasSamples = true;
  }

  if (Panel->HasSolidSamples) {
    char OverdrawText[DEBUG_TEXT_CAPACITY];
    snprintf(OverdrawText, sizeof(OverdrawText),
             "Solid Samples: %llu (%.2fx)",
             (unsigned long long)Panel->SolidSamples,
             (double)Panel->SolidOverdraw);
    Y = DrawDebugLine(Ctx, OverdrawText, Y);
    Y += FONT_LINE_HEIGHT;
  }

  Y = DrawWorldgenCountersLine(Ctx, Y);

  const char *const HEADER_CELLS[DEBUG_NUMBER_COLUMNS] = {"avg", "max", "1%low", "n"};
  Y = DrawTimingRow(Ctx, Y, "us/frame", HEADER_CELLS, "");

  enum { NUMBER_TEXT_SIZE = 16 };
  char Avg[NUMBER_TEXT_SIZE];
  char Max[NUMBER_TEXT_SIZE];
  char Low[NUMBER_TEXT_SIZE];
  char Count[NUMBER_TEXT_SIZE];

  // The whole-frame total first, so the scopes below read as parts of it and
  // the remainder they do not account for is apparent rather than inferred.
  snprintf(Avg, sizeof(Avg), "%u", Panel->FrameStats.AverageUs);
  snprintf(Max, sizeof(Max), "%u", Panel->FrameStats.MaxUs);
  snprintf(Low, sizeof(Low), "%u", Panel->FrameStats.OnePercentLowUs);
  const char *const FRAME_CELLS[DEBUG_NUMBER_COLUMNS] = {Avg, Max, Low, NULL};
  Y = DrawTimingRow(Ctx, Y, "Frame", FRAME_CELLS, "");

  for (int Scope = 0; Scope < PROFILE_SCOPE_COUNT; Scope++) {
    const ProfileStats *Stats = &Panel->Stats[Scope];
    // A worker-side scope is summed across those threads, so it can legitimately
    // exceed the frame's own duration. Marked so that reading it as a share of
    // the frame is not the obvious interpretation. Asked of the profiler rather
    // than hardcoded: mesh building moved to the workers, and a hardcoded test
    // would have kept labelling it as main-thread time.
    const char *Suffix =
        IsProfileScopeWorkerSide((ProfileScope)Scope) ? "(worker)" : "";
    snprintf(Avg, sizeof(Avg), "%u", Stats->AverageUs);
    snprintf(Max, sizeof(Max), "%u", Stats->MaxUs);
    snprintf(Low, sizeof(Low), "%u", Stats->OnePercentLowUs);
    snprintf(Count, sizeof(Count), "%u", Stats->AverageEntries);
    const char *const ROW_CELLS[DEBUG_NUMBER_COLUMNS] = {Avg, Max, Low, Count};
    Y = DrawTimingRow(Ctx, Y, GetProfileScopeName((ProfileScope)Scope),
                      ROW_CELLS, Suffix);
  }
  return Y;
}

#endif

static int DrawSimulationDebugLine(UiContext *Ctx, int Y) {
  const LaunchOptions *Options = GetLaunchOptions();
  bool HasWorkers = (Options->WorkerCount > 0);
  bool HasCores = (Options->CoreCount > 0);
  bool HasSlowdown = (Options->CpuSlowdown > 1.0);
  bool HasRam = (Options->RamLimitMb > 0);
  if (!HasWorkers && !HasCores && !HasSlowdown && !HasRam) {
    return Y;
  }

  char Line[DEBUG_TEXT_CAPACITY];
  int Pos = snprintf(Line, sizeof(Line), "SIM:");
  if (HasWorkers && Pos > 0 && (size_t)Pos < sizeof(Line)) {
    Pos += snprintf(Line + Pos, sizeof(Line) - (size_t)Pos, " workers %d",
                    Options->WorkerCount);
  }
  if (HasCores && Pos > 0 && (size_t)Pos < sizeof(Line)) {
    Pos += snprintf(Line + Pos, sizeof(Line) - (size_t)Pos, " cores %d",
                    Options->CoreCount);
  }
  if (HasSlowdown && Pos > 0 && (size_t)Pos < sizeof(Line)) {
    Pos += snprintf(Line + Pos, sizeof(Line) - (size_t)Pos, " cpu x%.1f",
                    Options->CpuSlowdown);
  }
  if (HasRam && Pos > 0 && (size_t)Pos < sizeof(Line)) {
    (void)snprintf(Line + Pos, sizeof(Line) - (size_t)Pos, " ram %zu MB",
                   Options->RamLimitMb);
  }

  return DrawDebugLine(Ctx, Line, Y);
}

static int FloorDiv(int Numerator, int Denominator) {
  int Quotient = Numerator / Denominator;
  int Remainder = Numerator % Denominator;
  if (Remainder != 0 && ((Remainder < 0) != (Denominator < 0))) {
    Quotient--;
  }
  return Quotient;
}

static void DrawDebugScreen(UiContext *Ctx, Player *PlayerVal, World *WorldVal,
                            GameCamera Camera) {
  char DebugText[DEBUG_TEXT_CAPACITY];
  int Y = DEBUG_MARGIN;

  int Fps = (int)(1.0F / PlatformGetFrameTime());
  snprintf(DebugText, sizeof(DebugText), "%d FPS", Fps);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  snprintf(DebugText, sizeof(DebugText), "XYZ: %.3f / %.5f / %.3f",
           (double)PlayerVal->Position.x, (double)PlayerVal->Position.y,
           (double)PlayerVal->Position.z);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  int BlockX = (int)floorf(PlayerVal->Position.x);
  int BlockY = (int)floorf(PlayerVal->Position.y);
  int BlockZ = (int)floorf(PlayerVal->Position.z);
  snprintf(DebugText, sizeof(DebugText), "Block: %d %d %d", BlockX, BlockY,
           BlockZ);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  int ChunkX = FloorDiv(BlockX, CHUNK_SIZE);
  int ChunkY = FloorDiv(BlockY, CHUNK_SIZE);
  int ChunkZ = FloorDiv(BlockZ, CHUNK_SIZE);
  snprintf(DebugText, sizeof(DebugText), "Chunk: %d %d %d", ChunkX, ChunkY,
           ChunkZ);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  Vec3 CamDir = Vec3Normalize(Vec3Sub(Camera.Target, Camera.Position));
  const char *FacingDir = (void *)0;
  const char *FacingAxis = (void *)0;
  if (fabsf(CamDir.x) > fabsf(CamDir.z)) {
    if (CamDir.x > 0.0F) {
      FacingDir = "East";
      FacingAxis = "Towards positive X";
    } else {
      FacingDir = "West";
      FacingAxis = "Towards negative X";
    }
  } else {
    if (CamDir.z > 0.0F) {
      FacingDir = "South";
      FacingAxis = "Towards positive Z";
    } else {
      FacingDir = "North";
      FacingAxis = "Towards negative Z";
    }
  }
  snprintf(DebugText, sizeof(DebugText), "Facing: %s (%s)", FacingDir,
           FacingAxis);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  uint64_t WorldSeed = GetWorldSeed();
  int BiomeId = SampleBiomeAt(BlockX, BlockY, BlockZ, WorldSeed, (void *)0);
  const BiomeType *BiomeDef = GetBiomeDef(BiomeId);
  const char *BiomeName =
      (BiomeDef != (void *)0 && BiomeDef->Id >= 0) ? BiomeDef->Name : "Unknown";
  snprintf(DebugText, sizeof(DebugText), "Biome: %s", BiomeName);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  if (PlayerVal->TargetBlock.Hit) {
    int Tx = (int)floorf(PlayerVal->TargetBlock.BlockPos.x);
    int Ty = (int)floorf(PlayerVal->TargetBlock.BlockPos.y);
    int Tz = (int)floorf(PlayerVal->TargetBlock.BlockPos.z);
    const BlockType *Def = GetBlockDef(PlayerVal->TargetBlock.BlockId);
    const char *Name = (Def != (void *)0 && Def->DisplayName[0] != '\0')
                           ? Def->DisplayName
                           : "Unknown";
    snprintf(DebugText, sizeof(DebugText),
             "Targeted Block: %s (ID: %d) at %d %d %d", Name,
             PlayerVal->TargetBlock.BlockId, Tx, Ty, Tz);
  } else {
    snprintf(DebugText, sizeof(DebugText), "Targeted Block: none");
  }
  Y = DrawDebugLine(Ctx, DebugText, Y);

  int ActiveChunks = 0;
  for (int Idx = 0; Idx < WorldVal->ChunkCount; Idx++) {
    if (WorldVal->SlotInUse[Idx] && WorldVal->Chunks[Idx].IsGenerated) {
      ActiveChunks++;
    }
  }
  snprintf(DebugText, sizeof(DebugText), "Active Chunks: %d / %d", ActiveChunks,
           MAX_ACTIVE_CHUNKS);
  Y = DrawDebugLine(Ctx, DebugText, Y);

  // Taken from the draw loop rather than recomputed: it already counted, and
  // re-running the visibility test here was a second full scan over every chunk.
  snprintf(DebugText, sizeof(DebugText), "Rendered Chunks: %d / %d",
           GetLastRenderedChunkCount(), MAX_ACTIVE_CHUNKS);
  Y = DrawDebugLine(Ctx, DebugText, Y);
  Y = DrawSimulationDebugLine(Ctx, Y);

#ifdef MINEGAME_PROFILE
  DrawTimingPanel(Ctx, Y + FONT_LINE_HEIGHT);
#else
  (void)Y;
#endif
}

void DrawHUD(UiContext *Ctx, Player *PlayerVal, World *WorldVal,
             GameCamera Camera, bool ShowDebugF3) {
  DrawHotbar(Ctx, PlayerVal);
  if (ShowDebugF3) {
    DrawDebugScreen(Ctx, PlayerVal, WorldVal, Camera);
  }
  DrawCrosshair(Ctx);
}
