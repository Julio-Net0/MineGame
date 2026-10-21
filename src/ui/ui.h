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
  UI_GUI_SCALE_MAX = 4,
  // Widget id meaning "no widget". Callers number their widgets from 1.
  UI_ID_NONE = 0
};

// Pointer state for one frame. UiBeginFrame takes it in window pixels and
// stores it in GUI units. The UI does not read the input library itself, so the
// caller converts whatever its input layer produced.
typedef struct {
  float X;
  float Y;
  bool LeftPressed;
  bool LeftDown;
  bool LeftReleased;
} UiPointer;

// One frame of text entry for a text field: UTF-8 bytes typed plus the edit
// edges.
typedef struct {
  const char *Chars;
  int Count;
  bool Backspace;
  bool Submit;
  bool Cancel;
} UiTextEvents;

typedef enum {
  UI_TEXT_NONE,
  UI_TEXT_SUBMIT,
  UI_TEXT_CANCEL
} UiTextAction;

typedef struct {
  UiDrawList DrawList;
  int Scale;
  int ScreenWidth;
  int ScreenHeight;
  int GuiWidth;
  int GuiHeight;
  UiScissor Scissor;
  UiPointer Pointer;
  // Hot is the widget under the pointer this frame; active is the one the
  // left button went down on, kept until the button is released, so a press
  // that drags off a button does not click whatever it is released over.
  int HotId;
  int ActiveId;
} UiContext;

// Half of a length, for centring.
#define UI_HALF 0.5F

static inline float UiClamp01(float Value) {
  if (Value < 0.0F) {
    return 0.0F;
  }
  return (Value > 1.0F) ? 1.0F : Value;
}

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

void UiBeginFrame(UiContext *Ctx, int ScreenWidth, int ScreenHeight,
                  const UiPointer *PointerPixels);
void UiEndFrame(UiContext *Ctx);

// Restricts everything emitted after it to Rect (GUI units) until
// UiClearScissor.
void UiSetScissor(UiContext *Ctx, UiRect Rect);
void UiClearScissor(UiContext *Ctx);

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

// ---------------------------------------------------------------------------
// Widgets. Each call draws the widget and returns its interaction in the same
// frame. Ids are caller-chosen, unique within a screen, never UI_ID_NONE.
// ---------------------------------------------------------------------------

// True on the frame the left button is released over the button it was
// pressed on. A disabled button draws greyed out and never reports a click.
bool UiButton(UiContext *Ctx, int Id, UiRect Rect, const char *Label,
              bool Enabled);

// Value is in [0, 1] and is updated while the handle is dragged; returns true
// on frames where it changed. Label is drawn centred on the track.
bool UiSlider(UiContext *Ctx, int Id, UiRect Rect, const char *Label,
              float *Value);

// Applies one frame of typing to Buffer, a NUL-terminated UTF-8 string of
// Capacity bytes including the terminator. Characters are appended and
// removed as whole sequences, so a full buffer or a backspace never leaves
// half a character. Returns whether the frame submitted or cancelled.
UiTextAction UiTextFieldEdit(char *Buffer, int Capacity,
                             const UiTextEvents *Events);

// Draws a text field showing Text, scrolled so its end stays visible and
// clipped to the field. Bordered fields use the vanilla text_field sprite;
// borderless ones (the chat line) a translucent dark strip.
void UiTextFieldDraw(UiContext *Ctx, UiRect Rect, const char *Text,
                     bool Focused, bool Bordered);

#endif
