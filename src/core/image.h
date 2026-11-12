#ifndef CORE_IMAGE_H
#define CORE_IMAGE_H

#include "core/color.h"
#include <stdbool.h>

enum {
  CORE_IMAGE_CHANNELS = 4
};

// Renderer-free RGBA8 image: Width * Height * CORE_IMAGE_CHANNELS bytes,
// row-major, top row first.
typedef struct {
  int Width;
  int Height;
  unsigned char *Pixels;
} CoreImage;

// Decode PNG bytes into RGBA8. On failure Out is zeroed and false is returned.
bool CoreImageDecodePng(const unsigned char *Data, int Size, CoreImage *Out);

// Write Image to Path as an RGBA8 PNG, creating or truncating the file. Returns
// false (and logs the path) when the image is empty or the file cannot be
// written; the image itself is never modified.
bool CoreImageEncodePng(const CoreImage *Image, const char *Path);

void CoreImageFree(CoreImage *Image);

// Image construction and editing for texture loading. Each allocates through
// the same allocator CoreImageFree releases, returns false (leaving an edited
// image untouched) when allocation fails or the arguments are out of range,
// and reproduces the arithmetic of the Raylib helper it replaced so textures
// built from it stay pixel-identical.

// Every pixel set to Color (GenImageColor).
bool CoreImageCreateFilled(CoreImage *Out, int Width, int Height, Color8 Color);

// Checkerboard of CheckWidth x CheckHeight cells, starting with First in the
// top-left cell (GenImageChecked).
bool CoreImageCreateChecked(CoreImage *Out, int Width, int Height,
                            int CheckWidth, int CheckHeight, Color8 First,
                            Color8 Second);

// Keeps only the given rectangle (ImageCrop with an in-bounds rectangle).
bool CoreImageCrop(CoreImage *Image, int X, int Y, int Width, int Height);

// Nearest-neighbour resample (ImageResizeNN, including its +1 ratio bias).
bool CoreImageResizeNearest(CoreImage *Image, int NewWidth, int NewHeight);

// Inflate a raw DEFLATE stream (zip method 8, no zlib header) into a heap
// buffer of exactly InflatedSize bytes (caller frees with free()). Returns null
// when the stream is corrupt or inflates to any other length; growth past a
// fixed ceiling fails, so a hostile stream cannot exhaust memory.
unsigned char *CoreInflateRaw(const unsigned char *Data, int CompressedLen,
                              int InflatedSize);

#endif
