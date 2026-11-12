#include "core/image.h"

#include "core/log.h"

#include <stdlib.h>
#include <string.h>

// Ceiling on any buffer stb_image grows while inflating. A corrupt or hostile
// DEFLATE stream (a zip bomb inside a resource pack) fails at this size instead
// of exhausting memory.
enum {
  MAX_INFLATE_GROWTH_BYTES = 64 * 1024 * 1024
};

static void *BoundedRealloc(void *Ptr, size_t NewSize) {
  if (NewSize > (size_t)MAX_INFLATE_GROWTH_BYTES) {
    return NULL;
  }
  return realloc(Ptr, NewSize);
}

#define STBI_MALLOC(Size) malloc(Size)
#define STBI_REALLOC(Ptr, NewSize) BoundedRealloc(Ptr, NewSize)
#define STBI_FREE(Ptr) free(Ptr)

// Raylib instantiates its own stb_image in rtextures.c with external linkage.
// STB_IMAGE_STATIC makes every function of this copy internal to this file, so
// the two copies cannot collide at link time. This file is the only place the
// vendored header may be included.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR

// A static instantiation leaves the loaders this file does not call unused.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "third_party/stb_image.h"
#pragma GCC diagnostic pop

// The PNG encoder is instantiated in its own translation unit; see the header
// for why it is not included here the way the loader above is.
#include "third_party/png_write.h"

bool CoreImageDecodePng(const unsigned char *Data, int Size, CoreImage *Out) {
  if (Out == NULL) {
    return false;
  }
  Out->Width = 0;
  Out->Height = 0;
  Out->Pixels = NULL;
  if (Data == NULL || Size <= 0) {
    return false;
  }

  int Width = 0;
  int Height = 0;
  int FileChannels = 0;
  stbi_uc *Pixels = stbi_load_from_memory(Data, Size, &Width, &Height,
                                          &FileChannels, CORE_IMAGE_CHANNELS);
  if (Pixels == NULL || Width <= 0 || Height <= 0) {
    stbi_image_free(Pixels);
    return false;
  }

  Out->Width = Width;
  Out->Height = Height;
  Out->Pixels = Pixels;
  return true;
}

bool CoreImageEncodePng(const CoreImage *Image, const char *Path) {
  if (Image == NULL || Path == NULL || Image->Pixels == NULL ||
      Image->Width <= 0 || Image->Height <= 0) {
    LogError("IMAGE: refusing to write an empty image to \"%s\"",
             (Path != NULL) ? Path : "(null)");
    return false;
  }

  int Stride = Image->Width * CORE_IMAGE_CHANNELS;
  if (!StbWritePng(Path, Image->Width, Image->Height, CORE_IMAGE_CHANNELS,
                   Image->Pixels, Stride)) {
    LogError("IMAGE: failed to write PNG \"%s\"", Path);
    return false;
  }
  return true;
}

void CoreImageFree(CoreImage *Image) {
  if (Image == NULL) {
    return;
  }
  stbi_image_free(Image->Pixels);
  Image->Pixels = NULL;
  Image->Width = 0;
  Image->Height = 0;
}

// Fraction bits of the nearest-neighbour resampler's source step.
enum {
  FIXED_SHIFT = 16
};

static unsigned char *AllocPixels(int Width, int Height) {
  if (Width <= 0 || Height <= 0) {
    return NULL;
  }
  return (unsigned char *)malloc((size_t)Width * (size_t)Height *
                                 (size_t)CORE_IMAGE_CHANNELS);
}

static void StorePixel(unsigned char *Pixel, Color8 Color) {
  Pixel[0] = Color.R;
  Pixel[1] = Color.G;
  Pixel[2] = Color.B;
  Pixel[3] = Color.A;
}

// Replaces Image's pixels with Pixels (already sized Width x Height).
static void AdoptPixels(CoreImage *Image, unsigned char *Pixels, int Width,
                        int Height) {
  free(Image->Pixels);
  Image->Pixels = Pixels;
  Image->Width = Width;
  Image->Height = Height;
}

bool CoreImageCreateFilled(CoreImage *Out, int Width, int Height, Color8 Color) {
  if (Out == NULL) {
    return false;
  }
  unsigned char *Pixels = AllocPixels(Width, Height);
  if (Pixels == NULL) {
    return false;
  }
  size_t Count = (size_t)Width * (size_t)Height;
  for (size_t Idx = 0; Idx < Count; Idx++) {
    StorePixel(&Pixels[Idx * CORE_IMAGE_CHANNELS], Color);
  }
  *Out = (CoreImage){.Width = Width, .Height = Height, .Pixels = Pixels};
  return true;
}

bool CoreImageCreateChecked(CoreImage *Out, int Width, int Height,
                            int CheckWidth, int CheckHeight, Color8 First,
                            Color8 Second) {
  if (Out == NULL || CheckWidth <= 0 || CheckHeight <= 0) {
    return false;
  }
  unsigned char *Pixels = AllocPixels(Width, Height);
  if (Pixels == NULL) {
    return false;
  }
  for (int Y = 0; Y < Height; Y++) {
    for (int X = 0; X < Width; X++) {
      bool IsFirst = ((X / CheckWidth) + (Y / CheckHeight)) % 2 == 0;
      size_t Offset = (((size_t)Y * (size_t)Width) + (size_t)X) *
                      (size_t)CORE_IMAGE_CHANNELS;
      StorePixel(&Pixels[Offset], IsFirst ? First : Second);
    }
  }
  *Out = (CoreImage){.Width = Width, .Height = Height, .Pixels = Pixels};
  return true;
}

bool CoreImageCrop(CoreImage *Image, int X, int Y, int Width, int Height) {
  if (Image == NULL || Image->Pixels == NULL || X < 0 || Y < 0 ||
      X + Width > Image->Width || Y + Height > Image->Height) {
    return false;
  }
  unsigned char *Pixels = AllocPixels(Width, Height);
  if (Pixels == NULL) {
    return false;
  }
  size_t RowBytes = (size_t)Width * (size_t)CORE_IMAGE_CHANNELS;
  for (int Row = 0; Row < Height; Row++) {
    size_t Src = ((((size_t)Y + (size_t)Row) * (size_t)Image->Width) +
                  (size_t)X) *
                 (size_t)CORE_IMAGE_CHANNELS;
    memcpy(&Pixels[(size_t)Row * RowBytes], &Image->Pixels[Src], RowBytes);
  }
  AdoptPixels(Image, Pixels, Width, Height);
  return true;
}

bool CoreImageResizeNearest(CoreImage *Image, int NewWidth, int NewHeight) {
  if (Image == NULL || Image->Pixels == NULL || Image->Width <= 0 ||
      Image->Height <= 0) {
    return false;
  }
  unsigned char *Pixels = AllocPixels(NewWidth, NewHeight);
  if (Pixels == NULL) {
    return false;
  }
  // 16.16 fixed-point source step. The +1 is Raylib's bias against an early
  // rounding error; it is kept because it decides which source texel each
  // output texel lands on, and textures must stay identical.
  int XRatio = ((int)((unsigned int)Image->Width << FIXED_SHIFT) / NewWidth) + 1;
  int YRatio = ((int)((unsigned int)Image->Height << FIXED_SHIFT) / NewHeight) + 1;
  for (int Y = 0; Y < NewHeight; Y++) {
    int SrcY = (Y * YRatio) >> FIXED_SHIFT;
    for (int X = 0; X < NewWidth; X++) {
      int SrcX = (X * XRatio) >> FIXED_SHIFT;
      size_t Src = (((size_t)SrcY * (size_t)Image->Width) + (size_t)SrcX) *
                   (size_t)CORE_IMAGE_CHANNELS;
      size_t Dst = (((size_t)Y * (size_t)NewWidth) + (size_t)X) *
                   (size_t)CORE_IMAGE_CHANNELS;
      memcpy(&Pixels[Dst], &Image->Pixels[Src], CORE_IMAGE_CHANNELS);
    }
  }
  AdoptPixels(Image, Pixels, NewWidth, NewHeight);
  return true;
}

unsigned char *CoreInflateRaw(const unsigned char *Data, int CompressedLen,
                              int InflatedSize) {
  if (Data == NULL || CompressedLen <= 0 || InflatedSize <= 0) {
    return NULL;
  }

  // Sized to the declared length up front, so a well-formed entry never grows
  // the buffer; growth only happens on a lying header and is capped above.
  int Written = 0;
  char *Out = stbi_zlib_decode_malloc_guesssize_headerflag(
      (const char *)Data, CompressedLen, InflatedSize, &Written, 0);
  if (Out == NULL) {
    return NULL;
  }
  if (Written != InflatedSize) {
    free(Out);
    return NULL;
  }
  return (unsigned char *)Out;
}
