#ifndef CORE_CAMERA_H
#define CORE_CAMERA_H

#include "core/vecmath.h"

// Engine-owned camera type. Renderer-agnostic: this header must never include a
// Raylib header. Mirrors RenderCamera's fields so the draw-time conversion is a
// trivial field copy. Perspective projection is implied (as the backend today).
typedef struct {
  Vec3 Position;
  Vec3 Target;
  Vec3 Up;
  float FovY;
} GameCamera;

#endif
