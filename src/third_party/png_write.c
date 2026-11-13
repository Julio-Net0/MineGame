#include "third_party/png_write.h"

#include <stdlib.h>

// This copy is static, so no other library linking its own stb_image_write
// can collide with it, and this file is the only place the vendored header may
// be included.
#define STBIW_MALLOC(Size) malloc(Size)
#define STBIW_REALLOC(Ptr, NewSize) realloc(Ptr, NewSize)
#define STBIW_FREE(Ptr) free(Ptr)
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION

// A static instantiation leaves the writers this file does not call unused.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "third_party/stb_image_write.h"
#pragma GCC diagnostic pop

bool StbWritePng(const char *Path, int Width, int Height, int Channels,
                 const void *Pixels, int Stride) {
  return stbi_write_png(Path, Width, Height, Channels, Pixels, Stride) != 0;
}
