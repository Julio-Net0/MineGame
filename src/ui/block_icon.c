#include "ui/block_icon.h"
#include "render/backend.h"
#include "world/biome.h"
#include "world/block_system.h"

#define BLOCK_ICON_HALF_DIVISOR 2.0F
#define BLOCK_ICON_QUART_DIVISOR 4.0F
#define BLOCK_ICON_SHADE_TOP 255U
#define BLOCK_ICON_SHADE_LEFT 150U
#define BLOCK_ICON_SHADE_RIGHT 200U
#define BLOCK_ICON_ALPHA 255U

typedef struct {
  float U0;
  float V0;
  float U1;
  float V1;
} IconUV;

typedef struct {
  float X;
  float Y;
} IconPoint;

static IconUV GetTileUV(int TextureIndex) {
  float Cell = 1.0F / (float)RENDER_BLOCK_ICON_GRID;
  int Column = TextureIndex % RENDER_BLOCK_ICON_GRID;
  int Row = TextureIndex / RENDER_BLOCK_ICON_GRID;
  float U = (float)Column * Cell;
  float V = (float)Row * Cell;
  return (IconUV){U, V, U + Cell, V + Cell};
}

static Color8 ShadeFace(Color8 Tint, unsigned int Shade) {
  Color8 Result;
  Result.R = (unsigned char)(((unsigned int)Tint.R * Shade) / BLOCK_ICON_SHADE_TOP);
  Result.G = (unsigned char)(((unsigned int)Tint.G * Shade) / BLOCK_ICON_SHADE_TOP);
  Result.B = (unsigned char)(((unsigned int)Tint.B * Shade) / BLOCK_ICON_SHADE_TOP);
  Result.A = BLOCK_ICON_ALPHA;
  return Result;
}

// The texture's top-left maps to P0 and its bottom-left to P1, so a side face
// reads upright.
static void EmitFace(UiContext *Ctx, IconUV Uv, Color8 Color, IconPoint P0,
                     IconPoint P1, IconPoint P2, IconPoint P3) {
  UiVertex Corners[UI_QUAD_CORNERS] = {
      {P0.X, P0.Y, Uv.U0, Uv.V0, Color},
      {P1.X, P1.Y, Uv.U0, Uv.V1, Color},
      {P2.X, P2.Y, Uv.U1, Uv.V1, Color},
      {P3.X, P3.Y, Uv.U1, Uv.V0, Color},
  };
  UiQuadGui(Ctx, UI_TEXTURE_BLOCK_ATLAS, Corners);
}

void UiBlockIcon(UiContext *Ctx, int BlockId, float X, float Y, float Size) {
  if (BlockId == 0) {
    return;
  }
  BlockType *Def = GetBlockDef(BlockId);
  Color8 Tint = GetBiomeTint(BIOME_FALLBACK_ID, Def->Tint);

  // Cross blocks (flora) have no cube geometry, so the isometric box icon is
  // wrong for them: draw their sprite as a flat, front-facing quad instead.
  if (Def->RenderType == BLOCK_RENDER_CROSS) {
    EmitFace(Ctx, GetTileUV(Def->TexTop), ShadeFace(Tint, BLOCK_ICON_SHADE_TOP),
             (IconPoint){X, Y}, (IconPoint){X, Y + Size},
             (IconPoint){X + Size, Y + Size}, (IconPoint){X + Size, Y});
    return;
  }

  IconUV TopUV = GetTileUV(Def->TexTop);
  IconUV SideUV = GetTileUV(Def->TexSide);
  bool HasSideOverlay = (Def->TexSideOverlay != NO_TEXTURE_OVERLAY);
  Color8 SideBaseTint = HasSideOverlay ? COLOR_WHITE : Tint;

  float Cx = X + (Size / BLOCK_ICON_HALF_DIVISOR);
  float Cy = Y + (Size / BLOCK_ICON_HALF_DIVISOR);
  float H = Size / BLOCK_ICON_QUART_DIVISOR;

  IconPoint PTop = {Cx, Y};
  IconPoint PTopLeft = {X, Y + H};
  IconPoint PTopRight = {X + Size, Y + H};
  IconPoint PCenter = {Cx, Cy};
  IconPoint PBottomLeft = {X, Cy + H};
  IconPoint PBottomRight = {X + Size, Cy + H};
  IconPoint PBottom = {Cx, Y + Size};

  EmitFace(Ctx, TopUV, ShadeFace(Tint, BLOCK_ICON_SHADE_TOP), PTop, PTopLeft,
           PCenter, PTopRight);
  EmitFace(Ctx, SideUV, ShadeFace(SideBaseTint, BLOCK_ICON_SHADE_LEFT),
           PTopLeft, PBottomLeft, PBottom, PCenter);
  EmitFace(Ctx, SideUV, ShadeFace(SideBaseTint, BLOCK_ICON_SHADE_RIGHT),
           PCenter, PBottom, PBottomRight, PTopRight);

  if (HasSideOverlay) {
    IconUV OverlayUV = GetTileUV(Def->TexSideOverlay);
    EmitFace(Ctx, OverlayUV, ShadeFace(Tint, BLOCK_ICON_SHADE_LEFT), PTopLeft,
             PBottomLeft, PBottom, PCenter);
    EmitFace(Ctx, OverlayUV, ShadeFace(Tint, BLOCK_ICON_SHADE_RIGHT), PCenter,
             PBottom, PBottomRight, PTopRight);
  }
}
