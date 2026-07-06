#ifndef RL_COMPAT_H
#define RL_COMPAT_H

#include "raylib.h"
#include "core/vecmath.h"

// Conversion between engine math types and Raylib math types. Lives on the
// Raylib side of the boundary (render/input/entry) so core/ stays Raylib-free.
// Conversion copies fields explicitly rather than reinterpret-casting.
static inline Vec3 Vec3FromRL(Vector3 V) { return (Vec3){V.x, V.y, V.z}; }
static inline Vector3 Vec3ToRL(Vec3 V) { return (Vector3){V.x, V.y, V.z}; }
static inline Vec2 Vec2FromRL(Vector2 V) { return (Vec2){V.x, V.y}; }
static inline Vector2 Vec2ToRL(Vec2 V) { return (Vector2){V.x, V.y}; }

#endif
