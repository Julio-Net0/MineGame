#include "ui/ui.h"
#include "core/log.h"
#include "render/backend.h"

enum {
  GUI_MIN_WIDTH = 320,
  GUI_MIN_HEIGHT = 240
};

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
}

void UiShutdown(void) {
  UiDrawListFree(&GetUiContext()->DrawList);
  GuiAtlasShutdown();
}

bool UiBuildAtlas(void) {
  CoreImage Atlas;
  bool Ok = GuiAtlasBuild(&Atlas);
  if (Ok) {
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

void UiBeginFrame(UiContext *Ctx, int ScreenWidth, int ScreenHeight) {
  UiDrawListReset(&Ctx->DrawList);
  Ctx->ScreenWidth = ScreenWidth;
  Ctx->ScreenHeight = ScreenHeight;
  Ctx->Scale =
      UiComputeGuiScale(ScreenWidth, ScreenHeight, UiGetGuiScaleSetting());
  Ctx->GuiWidth = ScreenWidth / Ctx->Scale;
  Ctx->GuiHeight = ScreenHeight / Ctx->Scale;
  Ctx->Scissor = (UiScissor){0};
}

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
