#ifndef THIRD_PARTY_PNG_WRITE_H
#define THIRD_PARTY_PNG_WRITE_H

#include <stdbool.h>

// Thin wrapper over the vendored stb_image_write, which is instantiated in its
// own translation unit (third_party/png_write.c) rather than in the caller's.
// Two reasons: Raylib instantiates the same library for its ExportImage, so
// this copy has to be static and therefore confined to one file; and the
// encoder's own code trips the clang static analyzer, which reports findings in
// headers against whichever file includes them regardless of clang-tidy's
// header filters. Keeping it here leaves the engine's own files clean to check.
//
// Writes Width x Height pixels of Channels bytes each, Stride bytes per row,
// to Path as a PNG. False on any failure; nothing is logged here.
bool StbWritePng(const char *Path, int Width, int Height, int Channels,
                 const void *Pixels, int Stride);

#endif
