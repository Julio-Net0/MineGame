#ifndef VECMATH_H
#define VECMATH_H

#include <math.h>

// Engine-owned math value types. Renderer-agnostic: this header must never
// include a Raylib header. Layout matches Raylib's Vector2/Vector3 so the
// render/input boundary can convert with an explicit field copy.
typedef struct {
  float x;
  float y;
} Vec2;

typedef struct {
  float x;
  float y;
  float z;
} Vec3;

static inline Vec3 Vec3Add(Vec3 A, Vec3 B) {
  return (Vec3){A.x + B.x, A.y + B.y, A.z + B.z};
}

static inline Vec3 Vec3Sub(Vec3 A, Vec3 B) {
  return (Vec3){A.x - B.x, A.y - B.y, A.z - B.z};
}

static inline Vec3 Vec3Scale(Vec3 A, float Scalar) {
  return (Vec3){A.x * Scalar, A.y * Scalar, A.z * Scalar};
}

static inline float Vec3Dot(Vec3 A, Vec3 B) {
  return (A.x * B.x) + (A.y * B.y) + (A.z * B.z);
}

static inline float Vec3Length(Vec3 A) {
  return sqrtf((A.x * A.x) + (A.y * A.y) + (A.z * A.z));
}

// Zero-length input is returned unchanged, matching raymath's Vector3Normalize.
static inline Vec3 Vec3Normalize(Vec3 A) {
  float Length = sqrtf((A.x * A.x) + (A.y * A.y) + (A.z * A.z));
  if (Length == 0.0F) {
    return A;
  }
  float InvLength = 1.0F / Length;
  return (Vec3){A.x * InvLength, A.y * InvLength, A.z * InvLength};
}

static inline Vec3 Vec3Cross(Vec3 A, Vec3 B) {
  return (Vec3){(A.y * B.z) - (A.z * B.y), (A.z * B.x) - (A.x * B.z),
                (A.x * B.y) - (A.y * B.x)};
}

#endif
