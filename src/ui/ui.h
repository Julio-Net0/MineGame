#ifndef UI_UI_H
#define UI_UI_H

#include "core/color.h"
#include "ui/draw_list.h"
#include "ui/gui_atlas.h"
#include <stdbool.h>

// Immediate-mode UI context. Layout is expressed in GUI units; emission
// multiplies by the integer GUI scale, so everything lands on whole screen
// pixels and the vanilla sprites stay sharp. The frame's output is a draw list
// the caller hands to the render backend.

// Rectangle in GUI units.
typedef struct {
  int X;
  int Y;
  int W;
  int H;
} UiRect;

enum {
  UI_GUI_SCALE_AUTO = 0,
  UI_GUI_SCALE_MAX = 4
};

typedef struct {
  UiDrawList DrawList;
  int Scale;
  int ScreenWidth;
  int ScreenHeight;
  int GuiWidth;
  int GuiHeight;
  UiScissor Scissor;
} UiContext;

UiContext *GetUiContext(void);

void UiInit(void);
void UiShutdown(void);

// Builds the GUI atlas from every sprite registered so far and uploads it.
// Call once, after every module has registered its sprites.
bool UiBuildAtlas(void);

// The setting is 0 (automatic) or a manual cap of 1..UI_GUI_SCALE_MAX.
void UiSetGuiScaleSetting(int Setting);
int UiGetGuiScaleSetting(void);

// Largest integer scale at which the screen is still at least 320x240 GUI
// units, capped by the setting when it is not automatic.
int UiComputeGuiScale(int ScreenWidth, int ScreenHeight, int Setting);

void UiBeginFrame(UiContext *Ctx, int ScreenWidth, int ScreenHeight);

void UiFillRect(UiContext *Ctx, UiRect Rect, Color8 Color);

// Draws the whole sprite stretched to Rect, ignoring any nine-slice metadata.
void UiSprite(UiContext *Ctx, UiSpriteId Sprite, UiRect Rect, Color8 Tint);

// Corners keep their border size; edges and centre tile the source, or the
// centre stretches when the sprite declares stretch_inner. Falls back to
// UiSprite for a sprite without nine-slice metadata.
void UiNineSlice(UiContext *Ctx, UiSpriteId Sprite, UiRect Rect, Color8 Tint);

// Axis-aligned quad in GUI units (fractional positions allowed) sampling an
// explicit UV rectangle of Texture.
void UiTexturedRect(UiContext *Ctx, UiTextureId Texture, float X, float Y,
                    float W, float H, float U0, float V0, float U1, float V1,
                    Color8 Color);

// Free-form quad with corners in GUI units (top-left, bottom-left,
// bottom-right, top-right), for geometry such as the isometric block icon.
void UiQuadGui(UiContext *Ctx, UiTextureId Texture,
               const UiVertex Corners[UI_QUAD_CORNERS]);

#endif
