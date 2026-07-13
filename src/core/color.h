#ifndef CORE_COLOR_H
#define CORE_COLOR_H

// Renderer-agnostic 8-bit RGBA color. This header must never include a Raylib
// header: 2D drawing expresses color and transparency through Color8 (its alpha
// channel), and the backend converts Color8 -> the renderer's color type at the
// boundary. Channel values are plain bytes, matching the MeshData color layout.
typedef struct {
  unsigned char R, G, B, A;
} Color8;

// Named constants used by the UI (values mirror the Raylib colors they replace).
#define COLOR_BLACK ((Color8){0, 0, 0, 255})
#define COLOR_WHITE ((Color8){255, 255, 255, 255})
#define COLOR_DARKGRAY ((Color8){80, 80, 80, 255})
#define COLOR_RAYWHITE ((Color8){245, 245, 245, 255})

// Returns Color with its alpha replaced by Factor in [0, 1] (replaces Fade).
static inline Color8 Color8Alpha(Color8 Color, float Factor) {
  Color.A = (unsigned char)(Factor * 255.0F);
  return Color;
}

#endif
