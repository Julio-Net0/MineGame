#ifndef UI_DRAW_LIST_H
#define UI_DRAW_LIST_H

#include "core/color.h"
#include <stdbool.h>

// Renderer-free 2D output of one frame: textured quads in screen pixels,
// grouped into commands that share a texture and a scissor rectangle. This is
// the whole contract between the UI and the render backend -- a backend only
// has to walk the commands, bind each texture, apply each scissor and draw the
// quads, which is what a future Vulkan backend will implement instead of every
// 2D primitive the UI knows about.

typedef enum {
  UI_TEXTURE_GUI_ATLAS = 0,
  UI_TEXTURE_BLOCK_ATLAS,
  UI_TEXTURE_COUNT
} UiTextureId;

typedef struct {
  float X;
  float Y;
  float U;
  float V;
  Color8 Color;
} UiVertex;

// Four free corners, not an axis-aligned rectangle, because the isometric
// block icon is drawn as parallelograms. Corner order is top-left, bottom-left,
// bottom-right, top-right.
enum {
  UI_QUAD_CORNERS = 4
};

typedef struct {
  UiVertex V[UI_QUAD_CORNERS];
} UiQuad;

// Screen pixels, top-left origin. A non-positive width means no scissor.
typedef struct {
  int X;
  int Y;
  int W;
  int H;
} UiScissor;

typedef struct {
  UiTextureId Texture;
  UiScissor Scissor;
  int FirstQuad;
  int QuadCount;
} UiCommand;

typedef struct {
  UiQuad *Quads;
  int QuadCount;
  int QuadCapacity;
  UiCommand *Commands;
  int CommandCount;
  int CommandCapacity;
} UiDrawList;

void UiDrawListFree(UiDrawList *List);

// Clears the counts and keeps the buffers, so a frame whose output fits the
// previous capacity allocates nothing.
void UiDrawListReset(UiDrawList *List);

// Appends Quad, starting a new command only when Texture or Scissor differs
// from the last one. False when the buffers could not grow; the quad is dropped.
bool UiDrawListPush(UiDrawList *List, UiTextureId Texture, UiScissor Scissor,
                    const UiQuad *Quad);

#endif
