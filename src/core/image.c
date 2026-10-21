#include "core/image.h"
#include <stdlib.h>

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

void CoreImageFree(CoreImage *Image) {
  if (Image == NULL) {
    return;
  }
  stbi_image_free(Image->Pixels);
  Image->Pixels = NULL;
  Image->Width = 0;
  Image->Height = 0;
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
