#ifndef RENDER_GL_MATH_H
#define RENDER_GL_MATH_H

#include "core/vecmath.h"
#include <math.h>
#include <string.h>

// Matrix helpers for the GL backend, ported from raylib 5.5 (rlgl.h and
// raymath.h, zlib/libpng licence, Copyright (c) Ramon Santamaria) with every
// expression kept in the original order and precision -- rlFrustum takes
// doubles and narrows them where raylib does. The golden screenshots were taken
// through raylib, so a matrix that differs in its last bit moves pixels on
// silhouette edges. Do not "simplify" the arithmetic here.

// Element names follow raylib's: m0..m3 is the first column. Fields are
// declared in that order, so the struct is laid out column-major exactly as
// glUniformMatrix4fv expects. Only designated initializers are used on it.
typedef struct {
  float m0, m1, m2, m3;
  float m4, m5, m6, m7;
  float m8, m9, m10, m11;
  float m12, m13, m14, m15;
} GlMat4;

enum {
  GL_MAT4_ELEMENTS = 16
};
_Static_assert(sizeof(GlMat4) == sizeof(float) * GL_MAT4_ELEMENTS,
               "GlMat4 must be a tightly packed float[16]");

// raylib's PI and DEG2RAD are float constants; the projection mixes them into
// double arithmetic exactly as BeginMode3D does.
#define GL_MATH_PI_F 3.14159265358979323846F
#define GL_MATH_DEG2RAD_F (GL_MATH_PI_F / 180.0F)
#define GL_MATH_CULL_NEAR 0.01
#define GL_MATH_CULL_FAR 1000.0
// Clip space spans two units per axis.
#define GL_MATH_CLIP_SPAN 2.0F
// The vertical field of view is split evenly about the view axis. Kept a
// double literal: raylib halves the float fovy in double precision.
#define GL_MATH_HALF_ANGLE 0.5

static inline GlMat4 GlMat4Identity(void) {
  return (GlMat4){.m0 = 1.0F, .m5 = 1.0F, .m10 = 1.0F, .m15 = 1.0F};
}

// rlMatrixMultiply / MatrixMultiply.
static inline GlMat4 GlMat4Multiply(GlMat4 Left, GlMat4 Right) {
  GlMat4 R = {0};
  R.m0 = (Left.m0*Right.m0) + (Left.m1*Right.m4) + (Left.m2*Right.m8) + (Left.m3*Right.m12);
  R.m1 = (Left.m0*Right.m1) + (Left.m1*Right.m5) + (Left.m2*Right.m9) + (Left.m3*Right.m13);
  R.m2 = (Left.m0*Right.m2) + (Left.m1*Right.m6) + (Left.m2*Right.m10) + (Left.m3*Right.m14);
  R.m3 = (Left.m0*Right.m3) + (Left.m1*Right.m7) + (Left.m2*Right.m11) + (Left.m3*Right.m15);
  R.m4 = (Left.m4*Right.m0) + (Left.m5*Right.m4) + (Left.m6*Right.m8) + (Left.m7*Right.m12);
  R.m5 = (Left.m4*Right.m1) + (Left.m5*Right.m5) + (Left.m6*Right.m9) + (Left.m7*Right.m13);
  R.m6 = (Left.m4*Right.m2) + (Left.m5*Right.m6) + (Left.m6*Right.m10) + (Left.m7*Right.m14);
  R.m7 = (Left.m4*Right.m3) + (Left.m5*Right.m7) + (Left.m6*Right.m11) + (Left.m7*Right.m15);
  R.m8 = (Left.m8*Right.m0) + (Left.m9*Right.m4) + (Left.m10*Right.m8) + (Left.m11*Right.m12);
  R.m9 = (Left.m8*Right.m1) + (Left.m9*Right.m5) + (Left.m10*Right.m9) + (Left.m11*Right.m13);
  R.m10 = (Left.m8*Right.m2) + (Left.m9*Right.m6) + (Left.m10*Right.m10) + (Left.m11*Right.m14);
  R.m11 = (Left.m8*Right.m3) + (Left.m9*Right.m7) + (Left.m10*Right.m11) + (Left.m11*Right.m15);
  R.m12 = (Left.m12*Right.m0) + (Left.m13*Right.m4) + (Left.m14*Right.m8) + (Left.m15*Right.m12);
  R.m13 = (Left.m12*Right.m1) + (Left.m13*Right.m5) + (Left.m14*Right.m9) + (Left.m15*Right.m13);
  R.m14 = (Left.m12*Right.m2) + (Left.m13*Right.m6) + (Left.m14*Right.m10) + (Left.m15*Right.m14);
  R.m15 = (Left.m12*Right.m3) + (Left.m13*Right.m7) + (Left.m14*Right.m11) + (Left.m15*Right.m15);
  return R;
}

// rlFrustum's matrix (before it is multiplied onto the current one).
static inline GlMat4 GlMat4Frustum(double Left, double Right, double Bottom,
                                   double Top, double ZNear, double ZFar) {
  GlMat4 M = {0};
  float Rl = (float)(Right - Left);
  float Tb = (float)(Top - Bottom);
  float Fn = (float)(ZFar - ZNear);

  M.m0 = ((float)ZNear * GL_MATH_CLIP_SPAN) / Rl;
  M.m5 = ((float)ZNear * GL_MATH_CLIP_SPAN) / Tb;
  M.m8 = ((float)Right + (float)Left) / Rl;
  M.m9 = ((float)Top + (float)Bottom) / Tb;
  M.m10 = -((float)ZFar + (float)ZNear) / Fn;
  M.m11 = -1.0F;
  M.m14 = -((float)ZFar * (float)ZNear * GL_MATH_CLIP_SPAN) / Fn;
  return M;
}

// rlOrtho's matrix (before it is multiplied onto the current one).
static inline GlMat4 GlMat4Ortho(double Left, double Right, double Bottom,
                                 double Top, double ZNear, double ZFar) {
  GlMat4 M = {0};
  float Rl = (float)(Right - Left);
  float Tb = (float)(Top - Bottom);
  float Fn = (float)(ZFar - ZNear);

  M.m0 = GL_MATH_CLIP_SPAN / Rl;
  M.m5 = GL_MATH_CLIP_SPAN / Tb;
  M.m10 = -GL_MATH_CLIP_SPAN / Fn;
  M.m12 = -((float)Left + (float)Right) / Rl;
  M.m13 = -((float)Top + (float)Bottom) / Tb;
  M.m14 = -((float)ZFar + (float)ZNear) / Fn;
  M.m15 = 1.0F;
  return M;
}

// MatrixLookAt.
static inline GlMat4 GlMat4LookAt(Vec3 Eye, Vec3 Target, Vec3 Up) {
  GlMat4 M = {0};

  Vec3 Vz = {Eye.x - Target.x, Eye.y - Target.y, Eye.z - Target.z};
  float Length = sqrtf((Vz.x*Vz.x) + (Vz.y*Vz.y) + (Vz.z*Vz.z));
  if (Length == 0.0F) { Length = 1.0F; }
  float ILength = 1.0F / Length;
  Vz.x *= ILength;
  Vz.y *= ILength;
  Vz.z *= ILength;

  Vec3 Vx = {(Up.y*Vz.z) - (Up.z*Vz.y), (Up.z*Vz.x) - (Up.x*Vz.z),
             (Up.x*Vz.y) - (Up.y*Vz.x)};
  Length = sqrtf((Vx.x*Vx.x) + (Vx.y*Vx.y) + (Vx.z*Vx.z));
  if (Length == 0.0F) { Length = 1.0F; }
  ILength = 1.0F / Length;
  Vx.x *= ILength;
  Vx.y *= ILength;
  Vx.z *= ILength;

  Vec3 Vy = {(Vz.y*Vx.z) - (Vz.z*Vx.y), (Vz.z*Vx.x) - (Vz.x*Vx.z),
             (Vz.x*Vx.y) - (Vz.y*Vx.x)};

  M.m0 = Vx.x;
  M.m1 = Vy.x;
  M.m2 = Vz.x;
  M.m4 = Vx.y;
  M.m5 = Vy.y;
  M.m6 = Vz.y;
  M.m8 = Vx.z;
  M.m9 = Vy.z;
  M.m10 = Vz.z;
  M.m12 = -((Vx.x*Eye.x) + (Vx.y*Eye.y) + (Vx.z*Eye.z));
  M.m13 = -((Vy.x*Eye.x) + (Vy.y*Eye.y) + (Vy.z*Eye.z));
  M.m14 = -((Vz.x*Eye.x) + (Vz.y*Eye.y) + (Vz.z*Eye.z));
  M.m15 = 1.0F;
  return M;
}

// The 3D view-projection BeginMode3D leaves behind for a perspective camera,
// combined the way the chunk pass and the line batch did: projection is the
// frustum multiplied onto identity, modelview is the view multiplied onto
// identity, and the result is MatrixMultiply(modelview, projection).
static inline GlMat4 GlMat4PerspectiveViewProjection(Vec3 Position, Vec3 Target,
                                                     Vec3 Up, float FovY,
                                                     int FramebufferWidth,
                                                     int FramebufferHeight) {
  float Aspect = (float)FramebufferWidth / (float)FramebufferHeight;
  double Top = GL_MATH_CULL_NEAR * tan(FovY * GL_MATH_HALF_ANGLE * GL_MATH_DEG2RAD_F);
  double Right = Top * Aspect;
  GlMat4 Projection = GlMat4Multiply(
      GlMat4Identity(),
      GlMat4Frustum(-Right, Right, -Top, Top, GL_MATH_CULL_NEAR, GL_MATH_CULL_FAR));
  GlMat4 ModelView =
      GlMat4Multiply(GlMat4LookAt(Position, Target, Up), GlMat4Identity());
  return GlMat4Multiply(ModelView, Projection);
}

// The 2D projection raylib's viewport setup establishes, top-left origin, with
// the identity modelview EndMode3D restores.
static inline GlMat4 GlMat4ScreenProjection(int Width, int Height) {
  double Left = 0.0;
  double Right = Width;
  double Bottom = Height;
  double Top = 0.0;
  GlMat4 Projection = GlMat4Multiply(
      GlMat4Identity(), GlMat4Ortho(Left, Right, Bottom, Top, 0.0F, 1.0F));
  return GlMat4Multiply(GlMat4Identity(), Projection);
}

static inline void GlMat4ToColumnMajor(GlMat4 M, float Out[GL_MAT4_ELEMENTS]) {
  memcpy(Out, &M, sizeof(float) * GL_MAT4_ELEMENTS);
}

#endif
