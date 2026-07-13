#include "input/input.h"
#include "core/camera.h"
#include "core/vecmath.h"

// Raylib-free input logic: derives the simulation's movement/interaction view
// from the engine camera. Raw polling stays in input/raylib_input.c.
PlayerView PlayerViewFromCamera(GameCamera Camera) {
  PlayerView View;
  View.EyePosition = Camera.Position;
  View.Forward = Vec3Normalize(Vec3Sub(Camera.Target, View.EyePosition));
  return View;
}
