#ifndef CAMERA_H
#define CAMERA_H

#include "raylib.h"
#include "core/vecmath.h"

Camera3D CreateGameCamera(void);

void UpdateGameCamera(Camera3D *Camera, Vector2 LookDelta);

// Snap the camera to the player's head position, preserving its view direction.
// Client-side presentation concern (not simulation).
void CameraFollowTarget(Camera3D *Camera, Vec3 PlayerPos, float HeadOffset);

#endif
