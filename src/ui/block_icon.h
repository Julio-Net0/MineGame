#ifndef UI_BLOCK_ICON_H
#define UI_BLOCK_ICON_H

#include "ui/ui.h"

// Draws a block's inventory icon at (X, Y) in GUI units, Size units square:
// an isometric cube with the top face lit and the two side faces shaded, or a
// flat sprite for cross-shaped flora. Tint and the grass side overlay match the
// block as it looks in the world. Samples the block-icon atlas.
void UiBlockIcon(UiContext *Ctx, int BlockId, float X, float Y, float Size);

#endif
