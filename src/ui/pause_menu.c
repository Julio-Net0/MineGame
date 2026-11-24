#include "ui/pause_menu.h"
#include "core/launch_options.h"
#include "platform/platform.h"
#include "render/lod_settings.h"
#include "ui/font.h"
#include "world/world.h"
#include <stdio.h>
#include <string.h>

// Vanilla's game menu layout, in GUI units: a column of 204-wide buttons
// starting a quarter of the way down, 24 units apart. Paired buttons split a
// row into two 100-wide halves with a 4-unit gap.
enum {
  MENU_BUTTON_WIDTH = 204,
  MENU_BUTTON_HEIGHT = 20,
  MENU_HALF_WIDTH = 100,
  MENU_ROW_PITCH = 24,
  MENU_TITLE_OFFSET = 20,
  // Space kept below the last row when the menu has to move up to fit: the
  // hotbar's height and a little more.
  MENU_BOTTOM_MARGIN = 26,
  MENU_LABEL_CAPACITY = 48,
  FOV_MIN = 30,
  FOV_MAX = 110,
  // Past 64 chunks the 1:1 far plane (1000 blocks) cuts the world and the LOD
  // takes over; --render-distance still reaches MAX_RENDER_DISTANCE.
  MENU_RENDER_DISTANCE_MAX = 64,
  // LOD distance steps: 0 is off, step k is k * LOD_DISTANCE_STEP chunks.
  LOD_DISTANCE_STEP = 128,
  LOD_DISTANCE_STEPS = LOD_DISTANCE_MAX / LOD_DISTANCE_STEP
};

enum {
  MENU_ROW_BACK = 0,
  MENU_ROW_DISPLAY,
  MENU_ROW_RENDER_DISTANCE,
  MENU_ROW_LOD_DISTANCE,
  MENU_ROW_LOD_QUALITY,
  MENU_ROW_QUIT,
  MENU_ROW_COUNT
};

enum {
  MENU_ID_BACK = 1,
  MENU_ID_GUI_SCALE,
  MENU_ID_FOV,
  MENU_ID_RENDER_DISTANCE,
  MENU_ID_LOD_DISTANCE,
  MENU_ID_LOD_QUALITY,
  MENU_ID_QUIT
};

#define MENU_DIM_COLOR ((Color8){16, 16, 16, 192})
#define MENU_TITLE_COLOR ((Color8){255, 255, 255, 255})

typedef struct {
  bool Open;
  bool QuitRequested;
  // Values shown while a slider is held. They are applied only on release,
  // so dragging across several values triggers one recompute rather than an
  // evict/enqueue pass per step.
  int PendingRenderDistance;
  bool RenderDistanceHeld;
  int PendingLodStep;
  bool LodDistanceHeld;
} PauseMenuState;

static PauseMenuState *GetPauseMenuState(void) {
  static PauseMenuState State;
  return &State;
}

void PauseMenuToggle(void) {
  PauseMenuState *State = GetPauseMenuState();
  State->Open = !State->Open;
  PlatformSetCursorDisabled(!State->Open);
}

void PauseMenuSetOpen(bool Open) {
  if (GetPauseMenuState()->Open != Open) {
    PauseMenuToggle();
  }
}

bool PauseMenuIsOpen(void) { return GetPauseMenuState()->Open; }

bool PauseMenuQuitRequested(void) {
  return GetPauseMenuState()->QuitRequested;
}

// Vanilla's position when the rows fit below it; otherwise as high as needed
// for the last row to stay on screen, never above the title's room.
static int FirstRowY(const UiContext *Ctx) {
  int Y = (Ctx->GuiHeight / 4) + MENU_ROW_PITCH;
  int Bottom = Y + ((MENU_ROW_COUNT - 1) * MENU_ROW_PITCH) + MENU_BUTTON_HEIGHT;
  int Limit = Ctx->GuiHeight - MENU_BOTTOM_MARGIN;
  if (Bottom > Limit) {
    Y -= Bottom - Limit;
  }
  return (Y < MENU_TITLE_OFFSET) ? MENU_TITLE_OFFSET : Y;
}

static UiRect MenuRow(const UiContext *Ctx, int Row) {
  int X = (Ctx->GuiWidth - MENU_BUTTON_WIDTH) / 2;
  int Y = FirstRowY(Ctx) + (Row * MENU_ROW_PITCH);
  return (UiRect){X, Y, MENU_BUTTON_WIDTH, MENU_BUTTON_HEIGHT};
}

static UiRect MenuHalf(const UiContext *Ctx, int Row, bool Right) {
  UiRect Rect = MenuRow(Ctx, Row);
  if (Right) {
    Rect.X += MENU_BUTTON_WIDTH - MENU_HALF_WIDTH;
  }
  Rect.W = MENU_HALF_WIDTH;
  return Rect;
}

static void CycleGuiScale(void) {
  int Next = UiGetGuiScaleSetting() + 1;
  UiSetGuiScaleSetting((Next > UI_GUI_SCALE_MAX) ? UI_GUI_SCALE_AUTO : Next);
}

static void DrawRenderDistanceSlider(UiContext *Ctx, PauseMenuState *State,
                                     UiRect Rect, int *RenderDistance) {
  if (!State->RenderDistanceHeld) {
    State->PendingRenderDistance = *RenderDistance;
  }

  float Range = (float)(MENU_RENDER_DISTANCE_MAX - MIN_RENDER_DISTANCE);
  float Value = UiClamp01(
      (float)(State->PendingRenderDistance - MIN_RENDER_DISTANCE) / Range);
  char Label[MENU_LABEL_CAPACITY];
  snprintf(Label, sizeof(Label), "Render Distance: %d",
           State->PendingRenderDistance);
  if (UiSlider(Ctx, MENU_ID_RENDER_DISTANCE, Rect, Label, &Value)) {
    State->PendingRenderDistance =
        MIN_RENDER_DISTANCE + (int)((Value * Range) + UI_HALF);
  }

  State->RenderDistanceHeld =
      Ctx->ActiveId == MENU_ID_RENDER_DISTANCE && Ctx->Pointer.LeftDown;
  if (!State->RenderDistanceHeld) {
    *RenderDistance = State->PendingRenderDistance;
  }
}

static void DrawLodDistanceSlider(UiContext *Ctx, PauseMenuState *State, UiRect Rect) {
  int Current = LodSettingsGet()->Distance;
  if (!State->LodDistanceHeld) {
    State->PendingLodStep = (Current + (LOD_DISTANCE_STEP / 2)) / LOD_DISTANCE_STEP;
  }
  float Value = (float)State->PendingLodStep / (float)LOD_DISTANCE_STEPS;
  int Shown = State->LodDistanceHeld ? State->PendingLodStep * LOD_DISTANCE_STEP
                                     : Current;
  char Label[MENU_LABEL_CAPACITY];
  if (Shown > 0) {
    snprintf(Label, sizeof(Label), "LOD Distance: %d", Shown);
  } else {
    snprintf(Label, sizeof(Label), "LOD Distance: Off");
  }
  if (UiSlider(Ctx, MENU_ID_LOD_DISTANCE, Rect, Label, &Value)) {
    State->PendingLodStep = (int)((Value * (float)LOD_DISTANCE_STEPS) + UI_HALF);
  }
  bool WasHeld = State->LodDistanceHeld;
  State->LodDistanceHeld =
      Ctx->ActiveId == MENU_ID_LOD_DISTANCE && Ctx->Pointer.LeftDown;
  int Chosen = State->PendingLodStep * LOD_DISTANCE_STEP;
  if (WasHeld && !State->LodDistanceHeld && Chosen != Current) {
    (void)LodSettingsSetDistance(Chosen);
  }
}

static void DrawLodControls(UiContext *Ctx, PauseMenuState *State) {
  const char *Reason = LodSettingsUnavailableReason();
  if (Reason != NULL) {
    // Shown disabled: it explains why the LOD rows do nothing here.
    char Label[MENU_LABEL_CAPACITY];
    int Prefix = snprintf(Label, sizeof(Label), "LOD: %s", Reason) -
                 (int)strlen(Reason);
    if (Prefix > 0 && Label[Prefix] >= 'a' && Label[Prefix] <= 'z') {
      Label[Prefix] = (char)(Label[Prefix] - 'a' + 'A');
    }
    (void)UiButton(Ctx, MENU_ID_LOD_DISTANCE, MenuRow(Ctx, MENU_ROW_LOD_DISTANCE),
                   Label, false);
    (void)UiButton(Ctx, MENU_ID_LOD_QUALITY, MenuRow(Ctx, MENU_ROW_LOD_QUALITY),
                   "LOD Quality: -", false);
    return;
  }
  DrawLodDistanceSlider(Ctx, State, MenuRow(Ctx, MENU_ROW_LOD_DISTANCE));
  LodQuality Quality = LodSettingsGet()->Quality;
  char Label[MENU_LABEL_CAPACITY];
  static const char *const NAMES[LOD_QUALITY_COUNT] = {"Low", "Medium", "High"};
  snprintf(Label, sizeof(Label), "LOD Quality: %s", NAMES[Quality]);
  if (UiButton(Ctx, MENU_ID_LOD_QUALITY, MenuRow(Ctx, MENU_ROW_LOD_QUALITY), Label,
               true)) {
    (void)LodSettingsSetQuality((LodQuality)(((int)Quality + 1) % LOD_QUALITY_COUNT));
  }
}

void DrawPauseMenu(UiContext *Ctx, GameCamera *Camera, int *RenderDistance) {
  PauseMenuState *State = GetPauseMenuState();
  if (!State->Open) {
    return;
  }

  UiFillRect(Ctx, (UiRect){0, 0, Ctx->GuiWidth, Ctx->GuiHeight},
             MENU_DIM_COLOR);

  const char *Title = "Game Menu";
  float TitleX = ((float)Ctx->GuiWidth - UiMeasureText(Title)) * UI_HALF;
  float TitleY = (float)(MenuRow(Ctx, MENU_ROW_BACK).Y - MENU_TITLE_OFFSET);
  UiTextShadow(Ctx, Title, TitleX, TitleY, MENU_TITLE_COLOR);

  if (UiButton(Ctx, MENU_ID_BACK, MenuRow(Ctx, MENU_ROW_BACK), "Back to Game", true)) {
    PauseMenuToggle();
    return;
  }

  char Label[MENU_LABEL_CAPACITY];
  int Setting = UiGetGuiScaleSetting();
  if (Setting == UI_GUI_SCALE_AUTO) {
    snprintf(Label, sizeof(Label), "GUI Scale: Auto");
  } else {
    snprintf(Label, sizeof(Label), "GUI Scale: %d", Setting);
  }
  if (UiButton(Ctx, MENU_ID_GUI_SCALE, MenuHalf(Ctx, MENU_ROW_DISPLAY, false), Label,
               true)) {
    CycleGuiScale();
  }

  float Range = (float)(FOV_MAX - FOV_MIN);
  float Value = UiClamp01((Camera->FovY - (float)FOV_MIN) / Range);
  snprintf(Label, sizeof(Label), "FOV: %d", (int)(Camera->FovY + UI_HALF));
  if (UiSlider(Ctx, MENU_ID_FOV, MenuHalf(Ctx, MENU_ROW_DISPLAY, true), Label, &Value)) {
    // Whole degrees, as vanilla's slider steps.
    Camera->FovY = (float)(FOV_MIN + (int)((Value * Range) + UI_HALF));
  }

  DrawRenderDistanceSlider(Ctx, State, MenuRow(Ctx, MENU_ROW_RENDER_DISTANCE),
                           RenderDistance);
  DrawLodControls(Ctx, State);

  if (UiButton(Ctx, MENU_ID_QUIT, MenuRow(Ctx, MENU_ROW_QUIT), "Save and Quit", true)) {
    State->QuitRequested = true;
  }
}
