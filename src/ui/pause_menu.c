#include "ui/pause_menu.h"
#include "platform/platform.h"
#include "ui/font.h"
#include <stdio.h>

// Vanilla's game menu layout, in GUI units: a column of 204-wide buttons
// starting a quarter of the way down, 24 units apart.
enum {
  MENU_BUTTON_WIDTH = 204,
  MENU_BUTTON_HEIGHT = 20,
  MENU_ROW_PITCH = 24,
  MENU_TITLE_OFFSET = 20,
  MENU_LABEL_CAPACITY = 32,
  FOV_MIN = 30,
  FOV_MAX = 110
};

enum {
  MENU_ID_BACK = 1,
  MENU_ID_GUI_SCALE,
  MENU_ID_FOV,
  MENU_ID_QUIT
};

#define MENU_DIM_COLOR ((Color8){16, 16, 16, 192})
#define MENU_TITLE_COLOR ((Color8){255, 255, 255, 255})

typedef struct {
  bool Open;
  bool QuitRequested;
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

bool PauseMenuIsOpen(void) { return GetPauseMenuState()->Open; }

bool PauseMenuQuitRequested(void) {
  return GetPauseMenuState()->QuitRequested;
}

static UiRect MenuRow(const UiContext *Ctx, int Row) {
  int X = (Ctx->GuiWidth - MENU_BUTTON_WIDTH) / 2;
  int Y = (Ctx->GuiHeight / 4) + MENU_ROW_PITCH + (Row * MENU_ROW_PITCH);
  return (UiRect){X, Y, MENU_BUTTON_WIDTH, MENU_BUTTON_HEIGHT};
}

static void CycleGuiScale(void) {
  int Next = UiGetGuiScaleSetting() + 1;
  UiSetGuiScaleSetting((Next > UI_GUI_SCALE_MAX) ? UI_GUI_SCALE_AUTO : Next);
}

void DrawPauseMenu(UiContext *Ctx, GameCamera *Camera) {
  PauseMenuState *State = GetPauseMenuState();
  if (!State->Open) {
    return;
  }

  UiFillRect(Ctx, (UiRect){0, 0, Ctx->GuiWidth, Ctx->GuiHeight},
             MENU_DIM_COLOR);

  const char *Title = "Game Menu";
  float TitleX = ((float)Ctx->GuiWidth - UiMeasureText(Title)) * UI_HALF;
  float TitleY = (float)(MenuRow(Ctx, 0).Y - MENU_TITLE_OFFSET);
  UiTextShadow(Ctx, Title, TitleX, TitleY, MENU_TITLE_COLOR);

  if (UiButton(Ctx, MENU_ID_BACK, MenuRow(Ctx, 0), "Back to Game", true)) {
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
  if (UiButton(Ctx, MENU_ID_GUI_SCALE, MenuRow(Ctx, 1), Label, true)) {
    CycleGuiScale();
  }

  float Range = (float)(FOV_MAX - FOV_MIN);
  float Value = UiClamp01((Camera->FovY - (float)FOV_MIN) / Range);
  snprintf(Label, sizeof(Label), "FOV: %d", (int)(Camera->FovY + UI_HALF));
  if (UiSlider(Ctx, MENU_ID_FOV, MenuRow(Ctx, 2), Label, &Value)) {
    // Whole degrees, as vanilla's slider steps.
    Camera->FovY = (float)(FOV_MIN + (int)((Value * Range) + UI_HALF));
  }

  if (UiButton(Ctx, MENU_ID_QUIT, MenuRow(Ctx, 3), "Save and Quit", true)) {
    State->QuitRequested = true;
  }
}
