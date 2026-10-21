#include "ui/ui.h"
#include "core/log.h"
#include "render/backend.h"
#include "ui/font.h"
#include "platform/platform.h"
#include <string.h>

enum {
  GUI_MIN_WIDTH = 320,
  GUI_MIN_HEIGHT = 240,
  BUTTON_LABEL_HEIGHT = 8,
  SLIDER_HANDLE_WIDTH = 8,
  TEXT_FIELD_INSET = 4,
  TEXT_FIELD_INSET_BORDERLESS = 2,
  CARET_BLINK_RATE = 2,
  UTF8_CONTINUATION_MASK = 0xC0,
  UTF8_CONTINUATION_TAG = 0x80,
  UTF8_TWO_MASK = 0xE0,
  UTF8_TWO_TAG = 0xC0,
  UTF8_THREE_MASK = 0xF0,
  UTF8_THREE_TAG = 0xE0,
  UTF8_FOUR_MASK = 0xF8,
  UTF8_FOUR_TAG = 0xF0
};

#define UI_LABEL_COLOR ((Color8){255, 255, 255, 255})
#define UI_LABEL_DISABLED_COLOR ((Color8){160, 160, 160, 255})
#define UI_TEXT_FIELD_BG_ALPHA 0.5F

typedef struct {
  UiSpriteId Button;
  UiSpriteId ButtonHighlighted;
  UiSpriteId ButtonDisabled;
  UiSpriteId Slider;
  UiSpriteId SliderHighlighted;
  UiSpriteId SliderHandle;
  UiSpriteId SliderHandleHighlighted;
  UiSpriteId TextField;
  UiSpriteId TextFieldHighlighted;
} WidgetSprites;

static WidgetSprites *GetWidgetSprites(void) {
  static WidgetSprites Sprites;
  return &Sprites;
}

static void RegisterWidgetSprites(void) {
  WidgetSprites *Sprites = GetWidgetSprites();
  Sprites->Button = GuiAtlasRegister("widget/button");
  Sprites->ButtonHighlighted = GuiAtlasRegister("widget/button_highlighted");
  Sprites->ButtonDisabled = GuiAtlasRegister("widget/button_disabled");
  Sprites->Slider = GuiAtlasRegister("widget/slider");
  Sprites->SliderHighlighted = GuiAtlasRegister("widget/slider_highlighted");
  Sprites->SliderHandle = GuiAtlasRegister("widget/slider_handle");
  Sprites->SliderHandleHighlighted =
      GuiAtlasRegister("widget/slider_handle_highlighted");
  Sprites->TextField = GuiAtlasRegister("widget/text_field");
  Sprites->TextFieldHighlighted =
      GuiAtlasRegister("widget/text_field_highlighted");
}

static int *GetGuiScaleSetting(void) {
  static int Setting = UI_GUI_SCALE_AUTO;
  return &Setting;
}

UiContext *GetUiContext(void) {
  static UiContext Ctx;
  return &Ctx;
}

void UiInit(void) {
  UiContext *Ctx = GetUiContext();
  *Ctx = (UiContext){0};
  Ctx->Scale = 1;
  RegisterWidgetSprites();
}

void UiShutdown(void) {
  UiDrawListFree(&GetUiContext()->DrawList);
  FontShutdown();
  GuiAtlasShutdown();
}

bool UiBuildAtlas(void) {
  CoreImage Atlas;
  bool Ok = GuiAtlasBuild(&Atlas);
  if (Ok) {
    // Glyph widths are measured from the packed pixels, which exist on the
    // CPU only until the upload below frees them.
    FontBuildMetrics(&Atlas);
    RenderUploadUiAtlas(&Atlas);
  }
  CoreImageFree(&Atlas);
  return Ok;
}

void UiSetGuiScaleSetting(int Setting) {
  if (Setting < UI_GUI_SCALE_AUTO) {
    Setting = UI_GUI_SCALE_AUTO;
  }
  if (Setting > UI_GUI_SCALE_MAX) {
    Setting = UI_GUI_SCALE_MAX;
  }
  *GetGuiScaleSetting() = Setting;
}

int UiGetGuiScaleSetting(void) { return *GetGuiScaleSetting(); }

int UiComputeGuiScale(int ScreenWidth, int ScreenHeight, int Setting) {
  int Scale = 1;
  while ((ScreenWidth / (Scale + 1)) >= GUI_MIN_WIDTH &&
         (ScreenHeight / (Scale + 1)) >= GUI_MIN_HEIGHT) {
    Scale++;
  }
  if (Setting > UI_GUI_SCALE_AUTO && Setting < Scale) {
    Scale = Setting;
  }
  return Scale;
}

void UiBeginFrame(UiContext *Ctx, int ScreenWidth, int ScreenHeight,
                  const UiPointer *PointerPixels) {
  UiDrawListReset(&Ctx->DrawList);
  Ctx->ScreenWidth = ScreenWidth;
  Ctx->ScreenHeight = ScreenHeight;
  Ctx->Scale =
      UiComputeGuiScale(ScreenWidth, ScreenHeight, UiGetGuiScaleSetting());
  Ctx->GuiWidth = ScreenWidth / Ctx->Scale;
  Ctx->GuiHeight = ScreenHeight / Ctx->Scale;
  Ctx->Scissor = (UiScissor){0};

  Ctx->Pointer = (PointerPixels != NULL) ? *PointerPixels : (UiPointer){0};
  Ctx->Pointer.X /= (float)Ctx->Scale;
  Ctx->Pointer.Y /= (float)Ctx->Scale;
  Ctx->HotId = UI_ID_NONE;
}

void UiEndFrame(UiContext *Ctx) {
  // Cleared after the widgets ran, so the release frame is still seen by the
  // widget that was pressed.
  if (!Ctx->Pointer.LeftDown) {
    Ctx->ActiveId = UI_ID_NONE;
  }
}

void UiSetScissor(UiContext *Ctx, UiRect Rect) {
  Ctx->Scissor = (UiScissor){Rect.X * Ctx->Scale, Rect.Y * Ctx->Scale,
                             Rect.W * Ctx->Scale, Rect.H * Ctx->Scale};
}

void UiClearScissor(UiContext *Ctx) { Ctx->Scissor = (UiScissor){0}; }

static void PushQuad(UiContext *Ctx, UiTextureId Texture, const UiQuad *Quad) {
  static bool ReportedFailure = false;
  if (!UiDrawListPush(&Ctx->DrawList, Texture, Ctx->Scissor, Quad) &&
      !ReportedFailure) {
    LogError("UI: out of memory growing the draw list, dropping quads");
    ReportedFailure = true;
  }
}

// Axis-aligned quad: Dst* in GUI units, UVs in atlas space.
static void EmitRect(UiContext *Ctx, UiTextureId Texture, float DstX,
                     float DstY, float DstW, float DstH, float U0, float V0,
                     float U1, float V1, Color8 Color) {
  float Scale = (float)Ctx->Scale;
  float X0 = DstX * Scale;
  float Y0 = DstY * Scale;
  float X1 = (DstX + DstW) * Scale;
  float Y1 = (DstY + DstH) * Scale;
  UiQuad Quad = {{
      {X0, Y0, U0, V0, Color},
      {X0, Y1, U0, V1, Color},
      {X1, Y1, U1, V1, Color},
      {X1, Y0, U1, V0, Color},
  }};
  PushQuad(Ctx, Texture, &Quad);
}

void UiTexturedRect(UiContext *Ctx, UiTextureId Texture, float X, float Y,
                    float W, float H, float U0, float V0, float U1, float V1,
                    Color8 Color) {
  EmitRect(Ctx, Texture, X, Y, W, H, U0, V0, U1, V1, Color);
}

void UiFillRect(UiContext *Ctx, UiRect Rect, Color8 Color) {
  float U = 0.0F;
  float V = 0.0F;
  GuiAtlasGetWhiteUV(&U, &V);
  EmitRect(Ctx, UI_TEXTURE_GUI_ATLAS, (float)Rect.X, (float)Rect.Y,
           (float)Rect.W, (float)Rect.H, U, V, U, V, Color);
}

void UiSprite(UiContext *Ctx, UiSpriteId Sprite, UiRect Rect, Color8 Tint) {
  const GuiSprite *Info = GuiAtlasGetSprite(Sprite);
  EmitRect(Ctx, UI_TEXTURE_GUI_ATLAS, (float)Rect.X, (float)Rect.Y,
           (float)Rect.W, (float)Rect.H, Info->U0, Info->V0, Info->U1,
           Info->V1, Tint);
}

void UiQuadGui(UiContext *Ctx, UiTextureId Texture,
               const UiVertex Corners[UI_QUAD_CORNERS]) {
  float Scale = (float)Ctx->Scale;
  UiQuad Quad;
  for (int Idx = 0; Idx < UI_QUAD_CORNERS; Idx++) {
    Quad.V[Idx] = Corners[Idx];
    Quad.V[Idx].X *= Scale;
    Quad.V[Idx].Y *= Scale;
  }
  PushQuad(Ctx, Texture, &Quad);
}

// ---------------------------------------------------------------------------
// Nine-slice
// ---------------------------------------------------------------------------

// A region of the sprite in logical units and the GUI-unit rectangle it fills.
typedef struct {
  int SrcX;
  int SrcY;
  int SrcW;
  int SrcH;
  int DstX;
  int DstY;
  int DstW;
  int DstH;
} SliceRegion;

static void EmitSlice(UiContext *Ctx, const GuiSprite *Info, int SrcX, int SrcY,
                      int SrcW, int SrcH, int DstX, int DstY, int DstW,
                      int DstH, Color8 Tint) {
  if (SrcW <= 0 || SrcH <= 0 || DstW <= 0 || DstH <= 0) {
    return;
  }
  float ScaleU = (Info->U1 - Info->U0) / (float)Info->LogicalWidth;
  float ScaleV = (Info->V1 - Info->V0) / (float)Info->LogicalHeight;
  float U0 = Info->U0 + ((float)SrcX * ScaleU);
  float V0 = Info->V0 + ((float)SrcY * ScaleV);
  float U1 = U0 + ((float)SrcW * ScaleU);
  float V1 = V0 + ((float)SrcH * ScaleV);
  EmitRect(Ctx, UI_TEXTURE_GUI_ATLAS, (float)DstX, (float)DstY, (float)DstW,
           (float)DstH, U0, V0, U1, V1, Tint);
}

// Fills the destination by repeating the source region whole, cropping the
// last repeat on each axis, which is how vanilla draws nine-slice edges and
// centres. An axis that does not tile stretches the source across it.
static void EmitTiled(UiContext *Ctx, const GuiSprite *Info, SliceRegion R,
                      bool TileX, bool TileY, Color8 Tint) {
  if (R.SrcW <= 0 || R.SrcH <= 0 || R.DstW <= 0 || R.DstH <= 0) {
    return;
  }
  int StepX = TileX ? R.SrcW : R.DstW;
  int StepY = TileY ? R.SrcH : R.DstH;
  for (int OffY = 0; OffY < R.DstH; OffY += StepY) {
    int DstH = (R.DstH - OffY < StepY) ? (R.DstH - OffY) : StepY;
    int SrcH = TileY ? DstH : R.SrcH;
    for (int OffX = 0; OffX < R.DstW; OffX += StepX) {
      int DstW = (R.DstW - OffX < StepX) ? (R.DstW - OffX) : StepX;
      int SrcW = TileX ? DstW : R.SrcW;
      EmitSlice(Ctx, Info, R.SrcX, R.SrcY, SrcW, SrcH, R.DstX + OffX,
                R.DstY + OffY, DstW, DstH, Tint);
    }
  }
}

// Shrinks two opposing borders proportionally when the target is smaller than
// their sum, so the corners exactly fill it.
static void FitBorders(int Size, int *Near, int *Far) {
  int Sum = *Near + *Far;
  if (Sum <= Size || Sum <= 0) {
    return;
  }
  *Near = (*Near * Size) / Sum;
  *Far = Size - *Near;
}

void UiNineSlice(UiContext *Ctx, UiSpriteId Sprite, UiRect Rect, Color8 Tint) {
  const GuiSprite *Info = GuiAtlasGetSprite(Sprite);
  if (Info->Scaling != GUI_SCALING_NINE_SLICE) {
    UiSprite(Ctx, Sprite, Rect, Tint);
    return;
  }

  int Left = Info->BorderLeft;
  int Right = Info->BorderRight;
  int Top = Info->BorderTop;
  int Bottom = Info->BorderBottom;
  FitBorders(Rect.W, &Left, &Right);
  FitBorders(Rect.H, &Top, &Bottom);

  int SrcW = Info->LogicalWidth;
  int SrcH = Info->LogicalHeight;
  int SrcCenterW = SrcW - Info->BorderLeft - Info->BorderRight;
  int SrcCenterH = SrcH - Info->BorderTop - Info->BorderBottom;
  int DstCenterW = Rect.W - Left - Right;
  int DstCenterH = Rect.H - Top - Bottom;
  int SrcRightX = SrcW - Right;
  int SrcBottomY = SrcH - Bottom;
  int DstRightX = Rect.X + Rect.W - Right;
  int DstBottomY = Rect.Y + Rect.H - Bottom;
  int DstInnerX = Rect.X + Left;
  int DstInnerY = Rect.Y + Top;

  // Corners.
  EmitSlice(Ctx, Info, 0, 0, Left, Top, Rect.X, Rect.Y, Left, Top, Tint);
  EmitSlice(Ctx, Info, SrcRightX, 0, Right, Top, DstRightX, Rect.Y, Right, Top,
            Tint);
  EmitSlice(Ctx, Info, 0, SrcBottomY, Left, Bottom, Rect.X, DstBottomY, Left,
            Bottom, Tint);
  EmitSlice(Ctx, Info, SrcRightX, SrcBottomY, Right, Bottom, DstRightX,
            DstBottomY, Right, Bottom, Tint);

  // Edges tile along their length.
  EmitTiled(Ctx, Info,
            (SliceRegion){Info->BorderLeft, 0, SrcCenterW, Top, DstInnerX,
                          Rect.Y, DstCenterW, Top},
            true, false, Tint);
  EmitTiled(Ctx, Info,
            (SliceRegion){Info->BorderLeft, SrcBottomY, SrcCenterW, Bottom,
                          DstInnerX, DstBottomY, DstCenterW, Bottom},
            true, false, Tint);
  EmitTiled(Ctx, Info,
            (SliceRegion){0, Info->BorderTop, Left, SrcCenterH, Rect.X,
                          DstInnerY, Left, DstCenterH},
            false, true, Tint);
  EmitTiled(Ctx, Info,
            (SliceRegion){SrcRightX, Info->BorderTop, Right, SrcCenterH,
                          DstRightX, DstInnerY, Right, DstCenterH},
            false, true, Tint);

  // Centre.
  bool Tile = !Info->StretchInner;
  EmitTiled(Ctx, Info,
            (SliceRegion){Info->BorderLeft, Info->BorderTop, SrcCenterW,
                          SrcCenterH, DstInnerX, DstInnerY, DstCenterW,
                          DstCenterH},
            Tile, Tile, Tint);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

static bool PointerInside(const UiContext *Ctx, UiRect Rect) {
  return Ctx->Pointer.X >= (float)Rect.X &&
         Ctx->Pointer.X < (float)(Rect.X + Rect.W) &&
         Ctx->Pointer.Y >= (float)Rect.Y &&
         Ctx->Pointer.Y < (float)(Rect.Y + Rect.H);
}

// Updates hot and active for a widget and returns whether it is hot.
static bool TrackWidget(UiContext *Ctx, int Id, UiRect Rect, bool Enabled) {
  bool Hot = Enabled && PointerInside(Ctx, Rect) &&
             (Ctx->ActiveId == UI_ID_NONE || Ctx->ActiveId == Id);
  if (Hot) {
    Ctx->HotId = Id;
    if (Ctx->Pointer.LeftPressed) {
      Ctx->ActiveId = Id;
    }
  }
  return Hot;
}

static void DrawCenteredLabel(UiContext *Ctx, UiRect Rect, const char *Label,
                              Color8 Color) {
  float Width = UiMeasureText(Label);
  float X = (float)Rect.X + (((float)Rect.W - Width) * UI_HALF);
  float Y = (float)Rect.Y + ((float)(Rect.H - BUTTON_LABEL_HEIGHT) * UI_HALF);
  UiTextShadow(Ctx, Label, X, Y, Color);
}

bool UiButton(UiContext *Ctx, int Id, UiRect Rect, const char *Label,
              bool Enabled) {
  const WidgetSprites *Sprites = GetWidgetSprites();
  bool Hot = TrackWidget(Ctx, Id, Rect, Enabled);
  bool Clicked = Hot && Ctx->ActiveId == Id && Ctx->Pointer.LeftReleased;

  UiSpriteId Sprite = Sprites->Button;
  if (!Enabled) {
    Sprite = Sprites->ButtonDisabled;
  } else if (Hot) {
    Sprite = Sprites->ButtonHighlighted;
  }
  UiNineSlice(Ctx, Sprite, Rect, COLOR_WHITE);
  DrawCenteredLabel(Ctx, Rect, Label,
                    Enabled ? UI_LABEL_COLOR : UI_LABEL_DISABLED_COLOR);
  return Clicked;
}

bool UiSlider(UiContext *Ctx, int Id, UiRect Rect, const char *Label,
              float *Value) {
  const WidgetSprites *Sprites = GetWidgetSprites();
  bool Hot = TrackWidget(Ctx, Id, Rect, true);
  bool Dragging = Ctx->ActiveId == Id && Ctx->Pointer.LeftDown;

  bool Changed = false;
  int Travel = Rect.W - SLIDER_HANDLE_WIDTH;
  if (Dragging && Travel > 0) {
    float Next = UiClamp01((Ctx->Pointer.X - (float)Rect.X -
                            ((float)SLIDER_HANDLE_WIDTH * UI_HALF)) /
                           (float)Travel);
    Changed = Next != *Value;
    *Value = Next;
  }

  bool Lit = Hot || Dragging;
  UiNineSlice(Ctx, Lit ? Sprites->SliderHighlighted : Sprites->Slider, Rect,
              COLOR_WHITE);
  int HandleX = Rect.X + (int)(*Value * (float)Travel);
  UiNineSlice(Ctx,
              Lit ? Sprites->SliderHandleHighlighted : Sprites->SliderHandle,
              (UiRect){HandleX, Rect.Y, SLIDER_HANDLE_WIDTH, Rect.H},
              COLOR_WHITE);
  DrawCenteredLabel(Ctx, Rect, Label, UI_LABEL_COLOR);
  return Changed;
}

static int Utf8SequenceLength(unsigned char Lead) {
  if ((Lead & UTF8_TWO_MASK) == UTF8_TWO_TAG) {
    return 2;
  }
  if ((Lead & UTF8_THREE_MASK) == UTF8_THREE_TAG) {
    return 3;
  }
  if ((Lead & UTF8_FOUR_MASK) == UTF8_FOUR_TAG) {
    return 4;
  }
  return 1;
}

UiTextAction UiTextFieldEdit(char *Buffer, int Capacity,
                             const UiTextEvents *Events) {
  if (Buffer == NULL || Capacity <= 0 || Events == NULL) {
    return UI_TEXT_NONE;
  }
  int Length = 0;
  while (Length < Capacity - 1 && Buffer[Length] != '\0') {
    Length++;
  }
  Buffer[Length] = '\0';

  int Idx = 0;
  while (Events->Chars != NULL && Idx < Events->Count) {
    int Sequence = Utf8SequenceLength((unsigned char)Events->Chars[Idx]);
    if (Sequence > Events->Count - Idx) {
      break;
    }
    if (Sequence <= Capacity - 1 - Length) {
      memcpy(&Buffer[Length], &Events->Chars[Idx], (size_t)Sequence);
      Length += Sequence;
      Buffer[Length] = '\0';
    }
    Idx += Sequence;
  }

  if (Events->Backspace && Length > 0) {
    Length--;
    while (Length > 0 && (((unsigned char)Buffer[Length]) &
                          UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_TAG) {
      Length--;
    }
    Buffer[Length] = '\0';
  }

  if (Events->Cancel) {
    return UI_TEXT_CANCEL;
  }
  return Events->Submit ? UI_TEXT_SUBMIT : UI_TEXT_NONE;
}

void UiTextFieldDraw(UiContext *Ctx, UiRect Rect, const char *Text,
                     bool Focused, bool Bordered) {
  const WidgetSprites *Sprites = GetWidgetSprites();
  int Inset = TEXT_FIELD_INSET_BORDERLESS;
  float TextY = (float)(Rect.Y + TEXT_FIELD_INSET_BORDERLESS);
  if (Bordered) {
    UiNineSlice(Ctx, Focused ? Sprites->TextFieldHighlighted : Sprites->TextField,
                Rect, COLOR_WHITE);
    Inset = TEXT_FIELD_INSET;
    TextY = (float)Rect.Y + ((float)(Rect.H - BUTTON_LABEL_HEIGHT) * UI_HALF);
  } else {
    UiFillRect(Ctx, Rect, Color8Alpha(COLOR_BLACK, UI_TEXT_FIELD_BG_ALPHA));
  }

  UiRect Inner = {Rect.X + Inset, Rect.Y, Rect.W - (Inset * 2), Rect.H};
  bool ShowCaret =
      Focused && ((int)(PlatformGetTime() * CARET_BLINK_RATE)) % 2 == 0;
  float TextWidth = UiMeasureText(Text);
  float CaretWidth = UiMeasureText("_");

  // Scrolled left just enough that the end of the text and the caret stay in
  // view, as vanilla's edit box does while typing.
  float Overflow = TextWidth + CaretWidth - (float)Inner.W;
  float TextX = (float)Inner.X - ((Overflow > 0.0F) ? Overflow : 0.0F);

  UiScissor Previous = Ctx->Scissor;
  UiSetScissor(Ctx, Inner);
  UiTextShadow(Ctx, Text, TextX, TextY, UI_LABEL_COLOR);
  if (ShowCaret) {
    UiTextShadow(Ctx, "_", TextX + TextWidth, TextY, UI_LABEL_COLOR);
  }
  Ctx->Scissor = Previous;
}
