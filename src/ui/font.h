#ifndef UI_FONT_H
#define UI_FONT_H

#include "core/color.h"
#include "core/image.h"
#include "ui/ui.h"

// Minecraft bitmap font, loaded from the resource-pack font definition
// (font/default.json and its references). Only bitmap and space providers are
// supported; unihex and ttf providers are skipped. Text is UTF-8 and every
// position and width is in GUI units.

enum {
  FONT_LINE_HEIGHT = 9
};

// Parses the font definition and registers its bitmap sheets with the GUI
// atlas. Must run before the atlas is built.
void FontInit(void);
void FontShutdown(void);

// Measures every glyph from the packed atlas pixels. Called by the UI between
// packing the atlas and uploading it.
void FontBuildMetrics(const CoreImage *Atlas);

void UiText(UiContext *Ctx, const char *Text, float X, float Y, Color8 Color);

// Vanilla drop shadow: the text drawn one unit right and down with its RGB
// divided by four, then the text itself.
void UiTextShadow(UiContext *Ctx, const char *Text, float X, float Y,
                  Color8 Color);

float UiMeasureText(const char *Text);

#endif
