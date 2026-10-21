#ifndef UI_GUI_ATLAS_H
#define UI_GUI_ATLAS_H

#include "core/image.h"
#include <stdbool.h>

// One packed texture holding every GUI sprite the UI asked for. Modules
// register sprites by their vanilla name relative to gui/sprites/ (for example
// "hud/hotbar") before GuiAtlasBuild; only registered sprites are decoded and
// packed, so startup cost follows what the UI draws, not the size of the asset
// folder.

typedef int UiSpriteId;

enum {
  UI_SPRITE_INVALID = -1
};

typedef enum {
  GUI_SCALING_NONE,
  GUI_SCALING_NINE_SLICE
} GuiScalingType;

// Logical sizes are in GUI units: the .mcmeta width/height when declared,
// otherwise the pixel size. A higher-resolution pack ships bigger pixels for
// the same logical size, so UVs are derived from the ratio and the layout is
// unchanged.
typedef struct {
  float U0;
  float V0;
  float U1;
  float V1;
  int LogicalWidth;
  int LogicalHeight;
  GuiScalingType Scaling;
  int BorderLeft;
  int BorderTop;
  int BorderRight;
  int BorderBottom;
  bool StretchInner;
} GuiSprite;

// Registering the same name twice returns the same id. Must be called before
// GuiAtlasBuild; later registrations return UI_SPRITE_INVALID.
UiSpriteId GuiAtlasRegister(const char *Name);

// Resolves, decodes and packs every registered sprite into OutAtlas (caller
// frees with CoreImageFree after uploading). A sprite missing from every
// source, or one that does not fit, draws as a magenta/black checker.
bool GuiAtlasBuild(CoreImage *OutAtlas);

// Never null: an invalid id returns the checker sprite.
const GuiSprite *GuiAtlasGetSprite(UiSpriteId Id);

// UV of the reserved opaque white texel that solid fills sample.
void GuiAtlasGetWhiteUV(float *OutU, float *OutV);

void GuiAtlasShutdown(void);

#endif
