#ifndef CORE_IMAGE_H
#define CORE_IMAGE_H

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

void CoreImageFree(CoreImage *Image);

// Inflate a raw DEFLATE stream (zip method 8, no zlib header) into a heap
// buffer of exactly InflatedSize bytes (caller frees with free()). Returns null
// when the stream is corrupt or inflates to any other length; growth past a
// fixed ceiling fails, so a hostile stream cannot exhaust memory.
unsigned char *CoreInflateRaw(const unsigned char *Data, int CompressedLen,
                              int InflatedSize);

#endif
